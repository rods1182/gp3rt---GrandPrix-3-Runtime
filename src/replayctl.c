/* GP3 1.13: replay controls like GP3 2000 (F1..F9) with an on-screen legend.
 *
 *   F1 previous lap    F2 next lap       F3 play / pause    F4 slow motion (held)
 *   F5 rewind (held)   F6 step back      F7 step forward    F8 fast forward (held)
 *   F9 show / hide the legend
 *   Shift+F1 start of the race    Shift+F2 where the race was paused
 *   (Home/End, Page Up/Down etc. are GP3 cameras; Shift+F-keys are used by neither GP3 nor GPxPatch)
 *
 * GP3 2000 keeps a snapshot of the race every few seconds and moves through them; 1.13 only
 * keeps a handful. Here, during the race, a snapshot is stored every second (packed: about
 * 3 KB each, a full one every 30 s), the way GP3 stores its own (state block 0xb93f84 +
 * 0x100 bytes at 0x54893c, pointers made relative by 0x47efc1 / back by 0x47f02c). In the
 * replay, rewind and fast forward go through them at 15x, and F1/F2 jump between laps.
 * They reach as far back as the replay input buffer: the whole race with GP3Replay.dll,
 * about 20 s without it (then the game's own [ and ] are used instead).
 * In the race loop (0x4062ff..0x406410) every pass reads the replay input (0x406372) and runs
 * one simulation step (0x4063de, the only place the clock moves); gates there make pause,
 * slow motion and single steps. Single steps back use a record of the last ~13 s, one entry
 * per step.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "gamepatch.h"
#include "replay_legend.h"

int plat_patch_code(void *addr, const void *bytes, int n);
void replayctl_inject_key(int vk, int scancode, int down);   /* platform: post a key to the game */
uint64_t rt_now_us(void);

#define CLOCK   (*(volatile int32_t *)(uintptr_t)0xb93fc4)
#define INREPLAY (*(volatile uint8_t *)(uintptr_t)0x55929e & 0x80)

enum { M_PLAY, M_PAUSE, M_SLOW, M_REW, M_FF, M_STEPB_REW, M_STEPB_FWD, M_START, M_STEPB_WAIT, M_END };
static int g_on, g_legend = 1, g_legend_built_ok = 1;
static volatile int g_mode = M_PLAY, g_steps, g_flash = -1, g_back;
static int g_held_vk;                 /* game key we are holding down (0x1a [ / 0x1b ]) */
static int32_t g_target, g_last_clock, g_still, g_frames, g_tries, g_live_clock, g_end;
static uint32_t g_slow_n;
static uint64_t g_last_gate_us, g_flash_until;
__attribute__((used)) uint8_t g_rc_run __asm__("gp3_rc_run") = 1;

static void hold(int vk) {
    if (g_held_vk == vk) return;
    if (g_held_vk) replayctl_inject_key(g_held_vk, g_held_vk == 0xdb ? 0x1a : 0x1b, 0);
    g_held_vk = vk;
    if (vk) replayctl_inject_key(vk, vk == 0xdb ? 0x1a : 0x1b, 1);
}
static void set_mode(int m) { g_mode = m; g_still = 0; if (m != M_REW && m != M_FF && m != M_END && m != M_STEPB_REW && m != M_STEPB_FWD && m != M_STEPB_WAIT && m != M_START) hold(0); }

/* our own record of the last seconds of the replay, one entry per simulation step, in the
   same form as GP3's snapshots (state block at 0xb93f84 + 0x100 bytes at 0x54893c, pointers
   made relative by 0x47efc1 / back by 0x47f02c): exact steps back without GP3's long
   rewind-and-replay. */
#define RING 320
static uint8_t *g_ring; static int32_t g_ring_clock[RING]; static int g_ring_head, g_ring_n; static uint32_t g_slen;
__attribute__((used)) volatile uint8_t g_rc_idle __asm__("gp3_rc_idle");
static void game_call(uint32_t fn) { __asm__ volatile("call *%0" :: "r"(fn) : "eax", "ecx", "edx", "memory", "cc"); }
static void put(const uint8_t *d);
static int ring_last(void) { return (g_ring_head + RING - 1) % RING; }
static void ring_save(int32_t c) {
    if (!g_ring) {
        g_slen = *(volatile uint32_t *)(uintptr_t)0xbb34f4;
        if (g_slen < 0x8000 || g_slen > 0x10000) return;
        g_ring = malloc((size_t)RING * (g_slen + 0x100)); if (!g_ring) return;
    }
    if (g_ring_n) { int32_t l = g_ring_clock[ring_last()]; if (c == l) return; if (c < l || c - l > 200) g_ring_n = 0; }   /* GP3 jumped: start again */
    uint8_t *d = g_ring + (size_t)g_ring_head * (g_slen + 0x100);
    game_call(0x47efc1);
    memcpy(d, (void *)(uintptr_t)0xb93f84, g_slen); memcpy(d + g_slen, (void *)(uintptr_t)0x54893c, 0x100);
    game_call(0x47f02c);
    g_ring_clock[g_ring_head] = c; g_ring_head = (g_ring_head + 1) % RING; if (g_ring_n < RING) g_ring_n++;
}
/* go back to the newest entry older than the clock (dropping it from the record); 0 if none */
static int ring_back(int32_t c) {
    while (g_ring_n) {
        int k = ring_last(); g_ring_head = k; g_ring_n--;
        if (g_ring_clock[k] >= c) continue;
        put(g_ring + (size_t)k * (g_slen + 0x100));
        return 1;
    }
    return 0;
}


/* ---- the per-second store (built during the race, used in the replay) ---- */
#define KEYEVERY 30
typedef struct { int32_t t; uint8_t lap; uint8_t key; uint32_t len; uint8_t *data; } snap_t;
static snap_t *g_st; static int g_st_n, g_st_cap, g_st_key = -1; static int32_t g_st_lastsec = -1, g_live_last;
static uint8_t *g_cur, *g_tmp; static size_t g_st_bytes;
static int32_t g_laps[512]; static int g_nlaps; static uint8_t g_lapmax;
static int32_t g_cap_ms;            /* how far back the replay input buffer reaches */
static int g_st_off;                /* store unusable (no memory) */

static void st_reset(void) {
    for (int i = 0; i < g_st_n; i++) free(g_st[i].data);
    g_st_n = 0; g_st_key = -1; g_st_lastsec = -1; g_nlaps = 0; g_lapmax = 0; g_st_bytes = 0;
}
static uint8_t lap_leader(void) {
    uint8_t m = 0; for (int i = 0; i < 22; i++) { uint8_t v = *(volatile uint8_t *)(uintptr_t)(0xb9556a + i); if (v > m && v < 200) m = v; }
    return m;
}
/* delta of cur against key: runs of (u16 skip, u16 len, bytes); skip 0xffff ends */
static uint32_t pack(const uint8_t *key, const uint8_t *cur, uint32_t n, uint8_t *out) {
    uint32_t i = 0, o = 0, last = 0;
    while (i < n) {
        if (key[i] == cur[i]) { i++; continue; }
        uint32_t j = i, same = 0;
        while (j < n && same < 8) { if (key[j] == cur[j]) same++; else same = 0; j++; }
        j -= same;
        uint32_t skip = i - last;
        while (skip > 0xfffe) { out[o++] = 0xfe; out[o++] = 0xff; out[o++] = 0; out[o++] = 0; skip -= 0xfffe; }
        out[o++] = skip & 0xff; out[o++] = skip >> 8; out[o++] = (j - i) & 0xff; out[o++] = (j - i) >> 8;
        memcpy(out + o, cur + i, j - i); o += j - i; i = last = j;
    }
    out[o++] = 0xff; out[o++] = 0xff;
    return o;
}
static void unpack(const uint8_t *in, uint8_t *dst) {
    uint32_t p = 0, at = 0;
    for (;;) {
        uint32_t skip = in[p] | in[p + 1] << 8; p += 2; if (skip == 0xffff) break;
        uint32_t len = in[p] | in[p + 1] << 8; p += 2;
        at += skip; memcpy(dst + at, in + p, len); p += len; at += len;
    }
}
static void grab(uint8_t *d) {
    game_call(0x47efc1);
    memcpy(d, (void *)(uintptr_t)0xb93f84, g_slen); memcpy(d + g_slen, (void *)(uintptr_t)0x54893c, 0x100);
    game_call(0x47f02c);
}
static void put(const uint8_t *d) {
    game_call(0x43b88a); game_call(0x47efc1);
    memcpy((void *)(uintptr_t)0xb93f84, d, g_slen); memcpy((void *)(uintptr_t)0x54893c, d + g_slen, 0x100);
    *(volatile uint16_t *)(uintptr_t)0xb27a2a = *(volatile uint16_t *)(uintptr_t)0xb94026;
    game_call(0x47f02c); game_call(0x43b897);
}
static int st_alloc(void) {   /* buffers and room for one more entry; 0 = store unusable */
    if (g_st_off) return 0;
    if (!g_slen) { g_slen = *(volatile uint32_t *)(uintptr_t)0xbb34f4; if (g_slen < 0x8000 || g_slen > 0x10000) { g_st_off = 1; return 0; } }
    uint32_t n = g_slen + 0x100;
    if (!g_cur) { g_cur = malloc(n); g_tmp = malloc(n * 2 + 64); if (!g_cur || !g_tmp) { g_st_off = 1; return 0; } }
    if (g_st_n == g_st_cap) {
        int nc = g_st_cap ? g_st_cap * 2 : 1024; snap_t *ns = realloc(g_st, nc * sizeof *ns);
        if (!ns) { g_st_off = 1; return 0; } g_st = ns; g_st_cap = nc;
    }
    return 1;
}
static int st_find(int32_t t);
#ifndef GP3_WIN
static snap_t *g_vs; static int g_vs_n;   /* test only */
#endif
static int32_t g_forget_at = -1;   /* test: forget the store at this race time, as after loading a saved race */
static void st_live(int32_t c) {
    if (g_st_off || g_cap_ms < 60000) return;
    if (c < g_live_last - 2000) st_reset();                        /* a new session */
    if (g_forget_at >= 0 && c >= g_forget_at) {
        g_forget_at = -1;
#ifndef GP3_WIN
        if (getenv("GP3_RC_VERIFY")) { g_vs = malloc(g_st_n * sizeof *g_vs); for (int i = 0; i < g_st_n; i++) { g_vs[i] = g_st[i]; g_vs[i].data = malloc(g_st[i].len); memcpy(g_vs[i].data, g_st[i].data, g_st[i].len); } g_vs_n = g_st_n; }
#endif
        st_reset(); LOG("replay keys: test: store forgotten at %d s", c / 1000);
    }
    g_live_last = c;
    int32_t sec = c / 1000;
    if (sec == g_st_lastsec) return;
    if (!st_alloc()) return;
    uint32_t n = g_slen + 0x100;
    /* keep only what the input buffer can still play: drop entries older than it (up to a keyframe) */
    for (;;) {
        int m = KEYEVERY; while (m < g_st_n && !g_st[m].key) m++;
        if (m >= g_st_n - KEYEVERY || g_st[m].t >= c - g_cap_ms + 5000) break;
        for (int i = 0; i < m; i++) { g_st_bytes -= g_st[i].len; free(g_st[i].data); }
        memmove(g_st, g_st + m, (g_st_n - m) * sizeof *g_st); g_st_n -= m; if (g_st_key >= 0) g_st_key -= m;
    }
    if (g_st_bytes > (size_t)300 << 20) { g_st_off = 1; LOG("replay keys: store full (%u MB)", (unsigned)(g_st_bytes >> 20)); return; }
    grab(g_cur);
    snap_t *e = &g_st[g_st_n];
    e->t = c; e->lap = lap_leader();
    if (g_st_key < 0 || g_st_n - g_st_key >= KEYEVERY) {
        e->key = 1; e->len = n; e->data = malloc(n); if (!e->data) { g_st_off = 1; return; } memcpy(e->data, g_cur, n); g_st_key = g_st_n;
    } else {
        e->key = 0; e->len = pack(g_st[g_st_key].data, g_cur, n, g_tmp); e->data = malloc(e->len); if (!e->data) { g_st_off = 1; return; } memcpy(e->data, g_tmp, e->len);
    }
    g_st_bytes += e->len;
    if (e->lap > g_lapmax) { if (g_nlaps < 512) g_laps[g_nlaps++] = c; g_lapmax = e->lap; }
    g_st_n++; g_st_lastsec = sec;
}
/* lap markers from the store: where the leader's lap goes up. After a gap (entries taken from
   GP3's own snapshots, ~32 s apart) the marker goes on the entry before, so a jump lands before
   the line and plays into the new lap. */
static void laps_rebuild(void) {
    g_nlaps = 0; g_lapmax = 0;
    for (int i = 0; i < g_st_n; i++) if (g_st[i].lap > g_lapmax) {
        g_lapmax = g_st[i].lap;
        if (i > 0 && g_nlaps < 512) g_laps[g_nlaps++] = g_st[i].t - g_st[i - 1].t > 1500 ? g_st[i - 1].t : g_st[i].t;
    }
}
/* put e at index at (room checked by st_alloc) */
static void st_insert(int at, const snap_t *e) {
    memmove(g_st + at + 1, g_st + at, (g_st_n - at) * sizeof *g_st);
    g_st[at] = *e; g_st_n++; g_st_bytes += e->len;
    if (g_st_key >= at) g_st_key++;
}
/* After loading a saved race the store only starts there, but GP3Replay's save keeps the whole
   replay: the input buffer and GP3's own snapshots (one every ~32 s, same form as ours, in the
   slots GP3Replay.dll gave it). When the replay starts, the part the store lacks is taken from
   those snapshots; then, as the replay plays through it, one entry per second is added
   (st_fill), so rewind and laps get as fine as in a race watched without loading. */
static void st_backfill(void) {
    if (g_cap_ms < 60000 || !st_alloc()) return;
    uint32_t base = *(volatile uint32_t *)(uintptr_t)0x47e7ca;                  /* GP3's snapshot slots (moved by GP3Replay.dll) */
    int32_t maxk = *(volatile int32_t *)(uintptr_t)0xb9fa38, hik = *(volatile int32_t *)(uintptr_t)0xb9fab4;
    if (*(volatile uint8_t *)(uintptr_t)0x47e7c9 != 0xbe || maxk < 3 || hik < 3 || hik > 1000) return;
    if (maxk > hik) maxk = hik;
    int32_t first = g_st_n ? g_st[0].t : g_end + 1, lo_t = g_end - g_cap_ms;
    uint32_t n = g_slen + 0x100; int got = 0;
    for (int k = 3; k <= maxk; k++) {
        const uint8_t *src = (const uint8_t *)(uintptr_t)(base + *(volatile uint32_t *)(uintptr_t)(0xbb34fc + 4 * k));
        int32_t t = *(const int32_t *)(src + 0x40);
        if (t < lo_t || t > g_end || t > first - 1500) continue;
        int i = st_find(t); if (i >= 0 && g_st[i].t == t) continue;
        if (!st_alloc()) break;
        snap_t e = { t, 0, 1, n, malloc(n) }; if (!e.data) break;
        memcpy(e.data, src, n);
        uint8_t m = 0; for (int j = 0; j < 22; j++) { uint8_t v = e.data[0xb9556a - 0xb93f84 + j]; if (v > m && v < 200) m = v; } e.lap = m;
        st_insert(i + 1, &e); got++;
    }
    if (got) { laps_rebuild(); LOG("replay keys: %d of GP3's snapshots added to the store (from %d s; the store started at %d s)", got, g_st[0].t / 1000, first / 1000); }
}
/* in the replay, at the point the store takes its entries: fill a gap with one entry per second */
static void st_fill(int32_t c) {
    if (g_st_off || g_st_n < 1 || c < g_end - g_cap_ms || c > g_end) return;
    int i = st_find(c);
    if (i < 0) { if (g_st[0].t - c < 1500) return; }
    else {
        if (g_st[i].t / 1000 == c / 1000 || i + 1 >= g_st_n) return;
        if (g_st[i + 1].t - g_st[i].t <= 1500 || g_st[i + 1].t / 1000 == c / 1000) return;
    }
    if (!st_alloc()) return;
    uint32_t n = g_slen + 0x100;
    grab(g_cur);
    int k = i; while (k >= 0 && !g_st[k].key) k--;
    /* a new keyframe only where it is nobody's base: the next entry is a keyframe too (or none) */
    int key = (k < 0 || i - k >= KEYEVERY - 1) && (i + 1 >= g_st_n || g_st[i + 1].key);
    if (k < 0 && !key) return;
    snap_t e = { c, lap_leader(), (uint8_t)key, 0, NULL };
    if (key) { e.len = n; e.data = malloc(n); if (e.data) memcpy(e.data, g_cur, n); }
    else { e.len = pack(g_st[k].data, g_cur, n, g_tmp); e.data = malloc(e.len); if (e.data) memcpy(e.data, g_tmp, e.len); }
    if (!e.data) { g_st_off = 1; return; }
    st_insert(i + 1, &e);
    if (e.lap != (i >= 0 ? g_st[i].lap : 0) || (i + 2 < g_st_n && g_st[i + 2].lap != e.lap)) laps_rebuild();
}
/* entries usable in this replay: [lo, hi] */
static int st_range(int *lo, int *hi) {
    if (g_st_off || g_st_n < 2) return 0;
    int l = 0, h = g_st_n - 1;
    while (l < g_st_n && g_st[l].t < g_end - g_cap_ms + 3000) l++;
    while (h >= 0 && g_st[h].t > g_end - 1500) h--;
    if (l > h) return 0;
    *lo = l; *hi = h; return 1;
}
static int st_find(int32_t t) {   /* last entry with time <= t, or -1 */
    int l = 0, h = g_st_n - 1, r = -1;
    while (l <= h) { int m = (l + h) / 2; if (g_st[m].t <= t) { r = m; l = m + 1; } else h = m - 1; }
    return r;
}
#ifndef GP3_WIN
/* test (GP3_RC_FORGET=<ms> GP3_RC_VERIFY=1): the store is kept aside when forgotten, and every
   replay step at one of its times is compared with it */
static int g_vf_n, g_vf_bad, g_vf_max;
static void rc_verify(int32_t c) {
    if (!g_vs_n || !g_cur) return;
    int l = 0, h = g_vs_n - 1, r = -1;
    while (l <= h) { int m = (l + h) / 2; if (g_vs[m].t <= c) { r = m; l = m + 1; } else h = m - 1; }
    if (r < 0 || g_vs[r].t != c) return;
    uint32_t n = g_slen + 0x100; static uint8_t *ref; if (!ref) ref = malloc(n);
    int k = r; while (k > 0 && !g_vs[k].key) k--;
    memcpy(ref, g_vs[k].data, n); if (k != r) unpack(g_vs[r].data, ref);
    grab(g_tmp);
    int d = 0, first = -1; for (uint32_t j = 0; j < n; j++) if (ref[j] != g_tmp[j]) { d++; if (first < 0) first = j; }
    g_vf_n++; if (d > 32) g_vf_bad++; if (d > g_vf_max) g_vf_max = d;
    if (d > 32 || (g_vf_n % 50) == 0) LOG("replay keys: test: %d s: %d bytes differ (first at +0x%x); checked %d, bad %d, max %d", c / 1000, d, first, g_vf_n, g_vf_bad, g_vf_max);
}
#else
static void rc_verify(int32_t c) { (void)c; }
#endif
static void st_restore(int i) {
    int k = i; while (k > 0 && !g_st[k].key) k--;
    memcpy(g_cur, g_st[k].data, g_slen + 0x100);
    if (k != i) unpack(g_st[i].data, g_cur);
    put(g_cur);
    g_ring_n = 0;
}

/* called every frame (game thread) instead of the simulation step; sets g_rc_run */
static int g_ret_mode = M_PLAY, g_keys_down;     /* mode to return to when a held key is released */
static int32_t g_v; static int g_vi = -1;         /* rewind / fast forward: virtual position and entry shown */
static int g_replay_no; static int g_rep, g_stepkey = -1; static volatile int g_jump; static uint64_t g_p_us;                                 /* F6/F7 held: repeat */
static void key_check(void);
static void lap_jump(int dir);
static int key_event(int vk, int down);
int plat_key_poll(int vk) __attribute__((weak));
static int g_polling;
__attribute__((used)) void rc_gate(void) __asm__("gp3_rc_gate");
static void cap_detect(void) {   /* how far back the replay input buffer reaches (7 bytes per 40 ms step; GP3Replay.dll makes it the whole race) */
    uint32_t st = *(uint32_t *)(uintptr_t)0x403be4, en = *(uint32_t *)(uintptr_t)0x403c27;
    g_cap_ms = en > st ? (int32_t)((en - st) / 7 * 40) : 0;
    LOG("replay keys: replay input buffer %d s%s", g_cap_ms / 1000, g_cap_ms >= 60000 ? " (rewind/fast forward through 1 s snapshots)" : " (game rewind/forward)");
}
__attribute__((used)) void rc_gate(void) {
    static int capd; if (!capd) { capd = 1; cap_detect(); }
    g_rc_run = 1;
    if (!INREPLAY) {
        if (g_held_vk) hold(0);
        g_mode = M_PLAY; g_steps = 0; g_live_clock = CLOCK; g_end = 0; g_ring_n = 0; g_vi = -1;
        return;
    }
    if (!g_end) {                       /* the replay starts: it ends where the race was when R was pressed */
        g_end = g_live_clock > CLOCK ? g_live_clock : CLOCK; if (g_end <= 0) g_end = 1; g_replay_no++;
        st_backfill();
        LOG("replay keys: replay %d starts at %d s, ends at %d s (store: %d entries from %d s)", g_replay_no, CLOCK / 1000, g_end / 1000, g_st_n, g_st_n ? g_st[0].t / 1000 : -1);
        *(volatile uint8_t *)(uintptr_t)0x55927a = 0;   /* P then R: start playing, not in GP3's pause */
        set_mode(M_PLAY); g_stepkey = -1; g_p_us = 0;
        if (!g_rc_idle) lap_jump(-1);                   /* start from the beginning of the lap, like F1 */
    }
    if (g_rc_idle && (*(volatile uint8_t *)(uintptr_t)0x55927a & 0x80) && !g_held_vk) {   /* GP3's own pause (P) in the replay: use ours */
        *(volatile uint8_t *)(uintptr_t)0x55927a = 0; if (rt_now_us() - g_p_us > 700000) set_mode(M_PAUSE); }   /* P itself is handled in replayctl_key */
    g_last_gate_us = rt_now_us();
    if (plat_key_poll) {   /* Windows: the game takes the keyboard with DirectInput; asking the system is reliable */
        static const int vks[10] = { 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 'P' };
        static uint8_t was[10]; static int polled_no;
        g_polling = 1;
        if (polled_no != g_replay_no) {   /* the replay just started: forget the P and R that opened it */
            polled_no = g_replay_no;
            for (int k = 0; k < 10; k++) was[k] = plat_key_poll(vks[k]) & 1;
            g_keys_down = 0;
        }
        for (int k = 0; k < 10; k++) {
            int st = plat_key_poll(vks[k]), now = st & 1, tapped = (st & 2) && !now && !was[k];
            if (now && !was[k]) key_event(vks[k], 1);
            if (!now && was[k]) key_event(vks[k], 0);
            if (tapped) { key_event(vks[k], 1); key_event(vks[k], 0); }
            was[k] = now;
        }
    } else key_check();
    int32_t c = CLOCK, moved = c != g_last_clock;
    if (!g_rc_idle && moved) { st_fill(c); rc_verify(c); }
    int lo, hi, have = !g_rc_idle && st_range(&lo, &hi);
    int jumped = 0;
    if (g_jump && have) { st_restore(g_jump - 1); g_jump = 0; c = CLOCK; jumped = 1; }   /* runs one step below, so the picture follows */
    switch (jumped ? -1 : g_mode) {
    case M_PAUSE:
        if (g_back && !g_rc_idle) {   /* one step back: go to the state before it and run one step, so it is drawn */
            g_back = 0;
            if (ring_back(c - 40)) { g_rc_run = 1; c = CLOCK; break; }
            if (have) { int i = st_find(c - 41); if (i >= lo) { st_restore(i); g_rc_run = 1; c = CLOCK; } else g_rc_run = 0; break; }
            g_target = c - 1; g_tries = 0; g_frames = 0; g_mode = M_STEPB_REW; break;
        }
        if (g_steps > 0) g_steps--; else g_rc_run = 0;
        break;
    case M_SLOW: g_rc_run = (g_slow_n++ % 4) == 0; break;
    case M_REW: case M_FF:
        if (have) {                                  /* through our store at 15x */
            if (g_vi == -1) { g_v = c; g_vi = -2; }   /* -2: started, nothing shown yet */
            g_v += g_mode == M_FF ? 600 : -600;
            if (g_v < g_st[lo].t) g_v = g_st[lo].t;
            if (g_v > g_st[hi].t) g_v = g_st[hi].t;
            int i = st_find(g_v); if (i < lo) i = lo; if (i > hi) i = hi;
            if (i != g_vi && (g_mode == M_REW ? g_st[i].t < c : g_st[i].t > c)) { st_restore(i); g_vi = i; c = CLOCK; g_rc_run = 1; }
            else g_rc_run = 0;
            break;
        }
        if (g_mode == M_REW) {                        /* no store: the game's own rewind */
            if (g_ring_n > 3 && !g_held_vk) { ring_back(c); ring_back(c - 40); ring_back(c - 80); g_rc_run = 0; g_still = 0; c = CLOCK; break; }
            hold(0xdb);
            if (!moved || c > g_last_clock) { if (++g_still > 15) set_mode(M_PAUSE); } else g_still = 0;
        } else {                                      /* no store: the game's ] (jumps between its snapshots) */
            static int slow;
            if (g_end > 0 && c >= g_end - 33000) { set_mode(M_PLAY); break; }
            if (!moved) { if (++g_still > 8) { set_mode(M_PAUSE); break; } } else g_still = 0;
            if (moved && c - g_last_clock < 120) slow++; else slow = 0;
            if (slow >= 6) { hold(0); slow = 0; break; }
            hold(0xdd);
        }
        break;
    case M_STEPB_REW:                 /* one step back without our records: a short press of the game's rewind... */
        hold(0xdb);
        if (++g_frames >= 3) { hold(0); g_mode = M_STEPB_WAIT; g_frames = 0; }
        break;
    case M_STEPB_WAIT:                /* ...wait until the replay plays normally again... */
        if (c > g_last_clock && c - g_last_clock < 200) g_frames++; else g_frames = 0;
        if (g_frames >= 5) {
            if (c < g_target) { g_mode = M_STEPB_FWD; g_frames = 0; }
            else if (++g_tries < 6) { g_mode = M_STEPB_REW; g_frames = 0; }
            else set_mode(M_PAUSE);
        }
        break;
    case M_STEPB_FWD:                 /* ...then fast forward and play up to the step before */
        if (c >= g_target) {
            if (c - g_target > 200 && ++g_tries < 6) { hold(0); g_mode = M_STEPB_REW; g_frames = 0; }
            else { set_mode(M_PAUSE); g_rc_run = 0; }
        }
        else hold(g_target - c > 1200 ? 0xdd : 0);
        break;
    }
    /* F6 / F7 held: repeat after a moment */
    if (g_rep && g_mode == M_PAUSE && !g_rc_idle && ++g_frames > 10) { if (g_rep < 0) g_back = 1; else g_steps = 1; }
    if (g_mode != M_REW && g_mode != M_FF) g_vi = -1;
    if (g_rc_run && !g_rc_idle && !g_held_vk) ring_save(CLOCK);
    g_last_clock = c;
}
/* two gates in the race loop (0x4062ff..0x406410), replay only: the replay input read
   (0x406372 -> 0x403bb0) and the simulation step at 0x4063de (-> 0x414d25, which moves the clock). When a frame is held,
   both are skipped and the loop only draws. */
__attribute__((used)) uint32_t g_rc_read __asm__("gp3_rc_read") = 0x403bb0;
__attribute__((used)) uint32_t g_rc_step __asm__("gp3_rc_step") = 0x414d25;
__attribute__((naked)) static void rc_read_stub(void) {
    __asm__ volatile("pushal\n call gp3_rc_gate\n popal\n"
                     "cmpb $0, gp3_rc_run\n je 1f\n jmp *gp3_rc_read\n 1: clc\n ret\n");
}
/* the live race records the player's input at 0x405f6d (-> 0x403c41): the store takes its
   snapshot right before, the same point where the replay reads it back */
__attribute__((used)) uint32_t g_rc_rec __asm__("gp3_rc_rec") = 0x403c41;
__attribute__((used)) void rc_live(void) __asm__("gp3_rc_live");
/* every live step (the race running): where the replay will end if R is pressed now (also without P first) */
__attribute__((used)) void rc_live(void) { if (g_on && !INREPLAY) { g_live_clock = CLOCK; st_live(g_live_clock); } }
__attribute__((naked)) static void rc_rec_stub(void) {
    __asm__ volatile("pushal\n call gp3_rc_live\n popal\n jmp *gp3_rc_rec\n");
}
/* the game's own "paused at the end of the replay" loop (0x40604e..0x40620e) also calls 0x414d25
   (at 0x4061d8): keep the keys and the legend alive there; the call itself always runs */
__attribute__((naked)) static void rc_idle_stub(void) {
    __asm__ volatile("pushal\n movb $1, gp3_rc_idle\n call gp3_rc_gate\n movb $0, gp3_rc_idle\n popal\n jmp *gp3_rc_step\n");
}
/* of the four calls GP3 itself skips while the race is paused (see 0x4061e7), three move debris,
   sparks, spray and smoke: a held frame skips them too (slow motion included). The first one
   (0x40977d) sets up the camera and keeps running, or the picture shakes. */
__attribute__((used)) uint32_t g_rc_anim[4] __asm__("gp3_rc_anim") = { 0x40977d, 0x4800dc, 0x47bdf6, 0x4730da };
#define ANIM_STUB(n, off) __attribute__((naked)) static void rc_anim_stub##n(void) { \
    __asm__ volatile("cmpb $0, gp3_rc_run\n je 1f\n jmp *gp3_rc_anim+" #off "\n 1: ret\n"); }
ANIM_STUB(0, 0) ANIM_STUB(1, 4) ANIM_STUB(2, 8) ANIM_STUB(3, 12)
__attribute__((naked)) static void rc_step_stub(void) {
    __asm__ volatile("cmpb $0, gp3_rc_run\n je 1f\n jmp *gp3_rc_step\n"
                     "1: ret\n");
}

/* GP3 draws nothing in a held frame but still flips: keep showing the last picture instead of
   rotating to the older back buffer (which made slow motion flicker) */
int replayctl_held_frame(void) {
    if (g_on && !INREPLAY) g_live_clock = CLOCK;   /* also when the race stands still (just loaded): the replay ends here */
    return g_on && !g_rc_run && INREPLAY;
}

int replayctl_active(void) { return g_on && INREPLAY && rt_now_us() - g_last_gate_us < 500000; }

/* keyboard from the window procedure; returns 1 when the key is ours (not passed to the game) */
/* is the key really down? Windows asks the system (a key-up message can get lost, e.g. when
   the window loses focus); elsewhere -1 = only our own record knows */
int plat_key_held(int vk) __attribute__((weak));
int plat_key_poll(int vk) __attribute__((weak));   /* bit 0 down now, bit 1 pressed since the last call */
static int key_held(int vk) { return plat_key_held ? plat_key_held(vk) : -1; }
/* our keys: 0..8 = F1..F9, 9 = Shift+F1 (start of the race), 10 = Shift+F2 (where the race was paused) */
static const int key_vk[11] = { 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x70, 0x71 };
static const int key_li[11] = { 1, 2, 4, 5, 6, 7, 8, 9, 10, 0, 3 };   /* place in the legend: ⇧F1 F1 F2 ⇧F2 F3 .. F9 */
static int key_index(int vk) { for (int f = 0; f < 9; f++) if (key_vk[f] == vk) return f; return -1; }
extern uint8_t g_keystate[256] __attribute__((weak));
static int shift_down(void) {
    if (plat_key_held) return plat_key_held(0x10) > 0;
    return &g_keystate[0] && ((g_keystate[0x10] | g_keystate[0xa0] | g_keystate[0xa1]) & 0x80);
}
static void key_release(int f) {
    g_keys_down &= ~(1 << f);
    if ((f == 3 && g_mode == M_SLOW) || (f == 4 && g_mode == M_REW) || (f == 7 && g_mode == M_FF)) set_mode(g_ret_mode);
    if (f == 5 || f == 6) g_rep = 0;
}
static void key_check(void) {   /* release what is no longer held */
    for (int f = 0; f < 11; f++) if ((g_keys_down & (1 << f)) && key_held(key_vk[f]) == 0) key_release(f);
    if ((g_keys_down & 0x10000) && key_held('P') == 0) g_keys_down &= ~0x10000;
}
static void lap_jump(int dir) {
    int lo, hi; if (!st_range(&lo, &hi)) { set_mode(dir < 0 ? M_REW : M_FF); return; }
    int32_t c = CLOCK, t = -1;
    if (dir < 0) { t = g_st[lo].t; for (int k = 0; k < g_nlaps; k++) if (g_laps[k] < c - 2000) t = g_laps[k] > t ? g_laps[k] : t; }
    else { t = g_st[hi].t; for (int k = g_nlaps - 1; k >= 0; k--) if (g_laps[k] > c + 500 && g_laps[k] < t) t = g_laps[k]; }
    int i = st_find(t); if (i < lo) i = lo; if (i > hi) i = hi;
    g_jump = i + 1;
}
int replayctl_key(int vk, int down) {
    if (!replayctl_active()) return 0;
    if (g_polling && (key_index(vk) >= 0 || vk == 'P')) return 1;   /* read by polling instead */
    return key_event(vk, down);
}
static int key_event(int vk, int down) {
    if (vk == 0xdb || vk == 0xdd) { if (down && vk != g_held_vk) set_mode(M_PLAY); return 0; }   /* the game's own keys */
    if (vk == 'R') return 1;                        /* R in the replay: nothing (GP3 leaves the replay at its end) */
    if (vk == 'P') {                                 /* P in the replay: our pause, like F3 (GP3's own pause there could not be left) */
        g_p_us = rt_now_us();
        if (down && (!(g_keys_down & 0x10000) || key_held('P') == 0)) { g_keys_down |= 0x10000; set_mode(g_mode == M_PAUSE ? M_PLAY : M_PAUSE); }
        if (!down) g_keys_down &= ~0x10000;
        return 1;
    }
    int f = key_index(vk);   /* 0 = F1 */
    if (f < 0) return 0;
    if (!down) { key_release(f); if (f < 2) key_release(9 + f); return 1; }
    if (f < 2 && shift_down()) f += 9;
    int bit = 1 << f;
    if ((g_keys_down & bit) && key_held(vk) != 0) return 1;   /* key repeat */
    g_keys_down |= bit;
    g_flash = f; g_flash_until = rt_now_us() + 350000;
    g_stepkey = (f == 5 || f == 6 || f == 3) ? f : -1;
    int base = (g_mode == M_PAUSE || g_mode == M_STEPB_REW || g_mode == M_STEPB_WAIT || g_mode == M_STEPB_FWD) ? M_PAUSE : M_PLAY;
    switch (f) {
    case 0: lap_jump(-1); break;
    case 1: lap_jump(1); break;
    case 2: set_mode(g_mode == M_PAUSE ? M_PLAY : M_PAUSE); break;
    case 3: if (base == M_PAUSE) { set_mode(M_PAUSE); g_steps++; } else { g_ret_mode = M_PLAY; set_mode(M_SLOW); } break;
    case 4: g_ret_mode = base; set_mode(M_REW); break;
    case 5: if (g_mode != M_PAUSE) set_mode(M_PAUSE); g_target = CLOCK - 1; g_back = 1; g_rep = -1; g_frames = 0; break;
    case 6: if (g_mode != M_PAUSE) set_mode(M_PAUSE); else g_steps++; g_rep = 1; g_frames = 0; break;
    case 7: g_ret_mode = base; set_mode(M_FF); break;
    case 8: g_legend = !g_legend; break;
    case 9: case 10: {   /* the whole way back / forward; End stops there, paused */
        int lo, hi; if (!st_range(&lo, &hi)) { set_mode(f == 9 ? M_REW : M_FF); break; }
        g_jump = (f == 9 ? lo : hi) + 1;
        if (f == 10) set_mode(M_PAUSE);
        break; }
    }
    return 1;
}

/* legend image (ARGB, RL_W x RL_H = 3x the game's pixels); NULL when it should not be shown */
const uint32_t *replayctl_legend(int *w, int *h, int *scale) {
    static uint32_t img[RL_W * RL_H]; static int built = -2;
    if (!g_legend || !replayctl_active()) return NULL;
    if (!g_legend_built_ok) return NULL;
    int lit = -1;
    switch (g_mode) {
    case M_PAUSE: lit = g_stepkey >= 0 ? g_stepkey : 2; break; case M_SLOW: lit = 3; break; case M_REW: lit = 4; break; case M_FF: lit = 7; break;
    case M_STEPB_REW: case M_STEPB_FWD: case M_STEPB_WAIT: lit = 5; break;
    }
    if (g_flash >= 0 && rt_now_us() < g_flash_until) lit = g_flash;
    if (lit != built) {
        for (int y = 0; y < RL_H; y++) for (int x = 0; x < RL_W; x++) {
            int i = y * RL_W + x, on = lit >= 0 && x >= rl_item[key_li[lit]][0] && x <= rl_item[key_li[lit]][1];
            unsigned bx = rl_box[i], t = rl_txt[i];
            if (bx < 128) { img[i] = 0; continue; }
            unsigned br = on ? 0xd0 : 0x16, bg = on ? 0x9a : 0x1a, bb = on ? 0x10 : 0x22;      /* box: dark, lit item amber */
            unsigned tr = on ? 0x10 : 0xff, tg = on ? 0x10 : 0xff, tb = on ? 0x10 : 0xff;      /* text: white, dark on amber */
            unsigned r = (br * (255 - t) + tr * t) / 255, g = (bg * (255 - t) + tg * t) / 255, b = (bb * (255 - t) + tb * t) / 255;
            img[i] = 0xff000000u | r << 16 | g << 8 | b;
        }
        built = lit;
    }
    *w = RL_W; *h = RL_H; *scale = RL_SCALE;
    return img;
}

void gp3_replay_keys(int legend) {
    const gp3_addrs_t *A = gp3_addrs();
    if (!A || A->fov_site != 0x4082fb) return;     /* GP3 1.13 only */
    static const uint8_t want_read[] = { 0xe8, 0x39, 0xd8, 0xff, 0xff }, want_step[] = { 0xe8, 0x42, 0xe9, 0x00, 0x00 };
    uint8_t *s1 = (uint8_t *)(uintptr_t)0x406372, *s2 = (uint8_t *)(uintptr_t)0x4063de;
    if (memcmp(s1, want_read, 5) || memcmp(s2, want_step, 5)) { LOG("replay keys: code differs, not applied"); return; }
    uint8_t c[5]; int32_t rel;
    c[0] = 0xe8; rel = (int32_t)((uintptr_t)rc_read_stub - (uintptr_t)(s1 + 5)); memcpy(c + 1, &rel, 4);
    if (plat_patch_code(s1, c, 5)) return;
    rel = (int32_t)((uintptr_t)rc_step_stub - (uintptr_t)(s2 + 5)); memcpy(c + 1, &rel, 4);
    if (plat_patch_code(s2, c, 5)) return;
    { static void (*const st[4])(void) = { rc_anim_stub0, rc_anim_stub1, rc_anim_stub2, rc_anim_stub3 };
      for (int k = 1; k < 4; k++) {   /* not 0x40977d: it sets up the camera, skipping it shakes the picture */
          uint32_t at = 0x4063ed + 5 * k;
          if (*(uint8_t *)(uintptr_t)at != 0xe8 || at + 5 + *(int32_t *)(uintptr_t)(at + 1) != g_rc_anim[k]) { LOG("replay keys: animation call %d differs", k); continue; }
          rel = (int32_t)((uintptr_t)st[k] - (uintptr_t)(at + 5)); memcpy(c + 1, &rel, 4); plat_patch_code((void *)(uintptr_t)at, c, 5);
      } }
    {   uint8_t *s5 = (uint8_t *)(uintptr_t)0x405f6d;
        if (!memcmp(s5, "\xe8\xcf\xdc\xff\xff", 5)) { rel = (int32_t)((uintptr_t)rc_rec_stub - (uintptr_t)(s5 + 5)); memcpy(c + 1, &rel, 4); plat_patch_code(s5, c, 5); }
        else LOG("replay keys: record call differs, no snapshot store"); }
    uint8_t *s3 = (uint8_t *)(uintptr_t)0x4061d8;
    if (!memcmp(s3, "\xe8\x48\xeb\x00\x00", 5)) { rel = (int32_t)((uintptr_t)rc_idle_stub - (uintptr_t)(s3 + 5)); memcpy(c + 1, &rel, 4); plat_patch_code(s3, c, 5); }
    g_on = 1; g_legend = legend;
#ifndef GP3_WIN
    if (getenv("GP3_RC_FORGET")) g_forget_at = atoi(getenv("GP3_RC_FORGET"));
#endif
    LOG("replay keys: on (F1..F9 like GP3 2000: laps, play/pause, slow, rewind, steps, fast forward; Shift+F1 / Shift+F2 start / end%s)", legend ? ", legend on screen" : "");
}
