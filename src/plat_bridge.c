/* Bridge platform for the Linux release: the game process (32-bit, static,
 * runs Gp3.exe) talks to the host process (64-bit: SDL2 window, OpenGL,
 * audio, input) through shared memory (bridge.h).
 *  - renderer calls are serialised into the command ring (remote backend)
 *  - input events arrive in the event ring and become window messages
 *  - presents are paced so the game never runs more than 2 frames ahead */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rt.h"
#include "plat.h"
#include "rdr.h"
#include "bridge.h"

br_shared_t *g_br;
static uint8_t *g_ring, *g_reply;
static pthread_mutex_t g_wmx = PTHREAD_MUTEX_INITIALIZER;
static int g_mode_w = 640, g_mode_h = 480;
static uint32_t g_frame;

#define A_LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define A_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define A_ADD(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)

/* ------------------------------------------------------------------ */
/* command ring                                                         */
typedef struct { const void *p; uint32_t len, rows, pitch; } seg_t;   /* rows > 0: rows of len bytes, pitch apart */
static uint32_t seg_total(const seg_t *s, int n) { uint32_t t = 0; for (int i = 0; i < n; i++) t += s[i].len * (s[i].rows ? s[i].rows : 1); return t; }
static uint32_t al8(uint32_t v) { return (v + 7) & ~7u; }

static void host_kick(void) {
    A_ADD(&g_br->cmd_wake, 1);
    if (A_LOAD(&g_br->host_waiting)) br_futex_wake(&g_br->cmd_wake);
}
/* wait until `need` contiguous bytes can be written; returns write offset */
static uint32_t ring_reserve(uint32_t need) {
    for (;;) {
        uint32_t head = g_br->head, tail = A_LOAD(&g_br->tail);
        uint32_t off = head & (BR_RING_SIZE - 1), tail_room = BR_RING_SIZE - off;
        uint32_t want = need + (tail_room < need ? tail_room : 0);
        if (BR_RING_SIZE - (head - tail) >= want + 8) {
            if (tail_room < need) {   /* pad to the start of the ring */
                br_hdr_t *h = (br_hdr_t *)(g_ring + off); h->op = BR_WRAP; h->len = tail_room - 8;
                A_STORE(&g_br->head, head + tail_room);
                continue;
            }
            return off;
        }
        host_kick();
        uint32_t sw = A_LOAD(&g_br->space_wake);
        A_STORE(&g_br->guest_waiting, 1);
        tail = A_LOAD(&g_br->tail);
        if (BR_RING_SIZE - (head - tail) < want + 8) br_futex_wait(&g_br->space_wake, sw, 20);
        A_STORE(&g_br->guest_waiting, 0);
        if (A_LOAD(&g_br->quit) > 1) _exit(0);
    }
}
static void put_segs(uint8_t *d, const seg_t *s, int n) {
    for (int i = 0; i < n; i++) {
        if (!s[i].rows) { memcpy(d, s[i].p, s[i].len); d += s[i].len; continue; }
        for (uint32_t r = 0; r < s[i].rows; r++) { memcpy(d, (const uint8_t *)s[i].p + (size_t)r * s[i].pitch, s[i].len); d += s[i].len; }
    }
}
/* one message; must be called with g_wmx held */
static void send_raw(uint32_t op, const seg_t *s, int n) {
    uint32_t len = seg_total(s, n), tot = 8 + al8(len);
    uint32_t off = ring_reserve(tot);
    br_hdr_t *h = (br_hdr_t *)(g_ring + off); h->op = op; h->len = al8(len);
    put_segs(g_ring + off + 8, s, n);
    A_STORE(&g_br->head, g_br->head + tot);
}
/* fixed part + optional bulk payload; bulk larger than a quarter ring goes through staging */
static void br_send(uint32_t op, const void *fixed, uint32_t flen, const seg_t *bulk, int nb) {
    uint32_t blen = bulk ? seg_total(bulk, nb) : 0;
    pthread_mutex_lock(&g_wmx);
    if (blen + flen <= BR_RING_SIZE / 4) {
        seg_t s[8]; int n = 0;
        if (flen) s[n++] = (seg_t){ fixed, flen, 0, 0 };
        for (int i = 0; i < nb && n < 8; i++) s[n++] = bulk[i];
        send_raw(op, s, n);
    } else {
        /* stream the bulk rows in chunks of at most 8 MB */
        uint8_t *tmp = malloc(8u << 20); uint32_t fill = 0;
        for (int i = 0; i < nb; i++) {
            uint32_t rows = bulk[i].rows ? bulk[i].rows : 1;
            for (uint32_t r = 0; r < rows; r++) {
                const uint8_t *p = (const uint8_t *)bulk[i].p + (size_t)r * bulk[i].pitch; uint32_t left = bulk[i].len;
                while (left) {
                    uint32_t k = (8u << 20) - fill; if (k > left) k = left;
                    memcpy(tmp + fill, p, k); fill += k; p += k; left -= k;
                    if (fill == (8u << 20)) { seg_t c = { tmp, fill, 0, 0 }; send_raw(BR_CHUNK, &c, 1); fill = 0; }
                }
            }
        }
        if (fill) { seg_t c = { tmp, fill, 0, 0 }; send_raw(BR_CHUNK, &c, 1); }
        free(tmp);
        seg_t f = { fixed, flen, 0, 0 };
        send_raw(op | BR_STAGED, &f, flen ? 1 : 0);
    }
    pthread_mutex_unlock(&g_wmx);
}
static void flush_kick(void) { host_kick(); }

/* synchronous request: returns pointer to reply data (valid until next request) */
static const uint8_t *br_request(uint32_t op, const void *fixed, uint32_t flen, uint32_t *rlen) {
    uint32_t seq = A_LOAD(&g_br->reply_seq);
    br_send(op, fixed, flen, NULL, 0);
    host_kick();
    while (A_LOAD(&g_br->reply_seq) == seq) {
        A_STORE(&g_br->guest_waiting, 1);
        if (A_LOAD(&g_br->reply_seq) == seq) br_futex_wait(&g_br->reply_seq, seq, 50);
        A_STORE(&g_br->guest_waiting, 0);
        if (A_LOAD(&g_br->quit) > 1) _exit(0);
    }
    if (rlen) *rlen = g_br->reply_len;
    return g_reply;
}

/* ------------------------------------------------------------------ */
/* remote renderer backend                                              */
struct rtex { uint32_t id; int w, h; };
struct rtarget { uint32_t id; int w, h; };
static uint32_t g_next_id = 1;
static pthread_mutex_t g_idmx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t new_id(void) { pthread_mutex_lock(&g_idmx); uint32_t v = g_next_id++; pthread_mutex_unlock(&g_idmx); return v; }

static rtex_t *r_tex_create(int w, int h, int levels) {
    rtex_t *t = calloc(1, sizeof *t); t->id = new_id(); t->w = w; t->h = h;
    uint32_t f[4] = { t->id, (uint32_t)w, (uint32_t)h, (uint32_t)levels };
    br_send(BR_TEXC, f, sizeof f, NULL, 0);
    return t;
}
static void r_tex_upload(rtex_t *t, int level, int w, int h, const uint32_t *argb) {
    uint32_t f[4] = { t->id, (uint32_t)level, (uint32_t)w, (uint32_t)h };
    seg_t b = { argb, (uint32_t)w * h * 4, 0, 0 };
    br_send(BR_TEXU, f, sizeof f, &b, 1);
}
static void r_tex_destroy(rtex_t *t) { uint32_t f[1] = { t->id }; br_send(BR_TEXD, f, 4, NULL, 0); free(t); }
static rtarget_t *r_target_create(int w, int h, int scale) {
    rtarget_t *t = calloc(1, sizeof *t); t->id = new_id(); t->w = w; t->h = h;
    uint32_t f[4] = { t->id, (uint32_t)w, (uint32_t)h, (uint32_t)scale };
    br_send(BR_TGTC, f, sizeof f, NULL, 0);
    return t;
}
static void r_target_destroy(rtarget_t *t) { uint32_t f[1] = { t->id }; br_send(BR_TGTD, f, 4, NULL, 0); free(t); }
static void r_target_upload(rtarget_t *t, const uint32_t *argb, int pitch, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    uint32_t f[5] = { t->id, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    seg_t b = { argb + (size_t)y * pitch + x, (uint32_t)w * 4, (uint32_t)h, (uint32_t)pitch * 4 };
    br_send(BR_TGTU, f, sizeof f, &b, 1);
}
static void r_target_download(rtarget_t *t, uint32_t *argb, int pitch, int y0, int n) {
    int per = (int)(BR_REPLY_SIZE / ((uint32_t)t->w * 4)); if (per < 1) per = 1;
    while (n > 0) {
        int k = n < per ? n : per;
        uint32_t f[3] = { t->id, (uint32_t)y0, (uint32_t)k }, rl = 0;
        const uint8_t *r = br_request(BR_TGTDL, f, sizeof f, &rl);
        for (int j = 0; j < k && (uint32_t)(j + 1) * t->w * 4 <= rl; j++)
            memcpy(argb + (size_t)(y0 + j) * pitch, r + (size_t)j * t->w * 4, (size_t)t->w * 4);
        y0 += k; n -= k;
    }
}
static void r_clear(rtarget_t *t, const int *rects, int nrects, int color, uint32_t argb, int depth, float z, int vx, int vy, int vw, int vh) {
    uint32_t f[2] = { t->id, (uint32_t)nrects };
    uint32_t g[8]; g[0] = (uint32_t)color; g[1] = argb; g[2] = (uint32_t)depth; memcpy(&g[3], &z, 4);
    g[4] = (uint32_t)vx; g[5] = (uint32_t)vy; g[6] = (uint32_t)vw; g[7] = (uint32_t)vh;
    seg_t b[2] = { { rects, (uint32_t)nrects * 16, 0, 0 }, { g, sizeof g, 0, 0 } };
    br_send(BR_CLEAR, f, sizeof f, b, 2);
}
static void wire_state(br_state_t *w, const rstate_t *s) {
    memset(w, 0, sizeof *w);
#define C(n) w->n = s->n;
    C(zenable) C(zwrite) C(zfunc) C(cull) C(blend) C(srcblend) C(dstblend) C(atest) C(afunc) C(aref) C(fog) C(fogtable) C(fogcolor)
    C(fogstart) C(fogend) C(fogdensity) C(flat) C(specular) C(ckey) C(dither) C(tfactor) C(zbias) C(nstages) C(vx) C(vy) C(vw) C(vh) C(minz) C(maxz)
#undef C
    for (int i = 0; i < 2; i++) {
        const rstage_t *a = &s->st[i]; br_stage_t *b = &w->st[i];
        b->tex = a->tex ? a->tex->id : 0;
#define C(n) b->n = a->n;
        C(has_alpha) C(has_ckey) C(colorop) C(carg1) C(carg2) C(alphaop) C(aarg1) C(aarg2) C(texcoord) C(addru) C(addrv) C(magf) C(minf) C(mipf) C(border) C(lodbias)
#undef C
    }
}
static void r_draw(rtarget_t *t, const rstate_t *s, int prim, const rvert_t *v, int nv, const uint16_t *idx, int ni) {
    struct { uint32_t id; br_state_t st; uint32_t prim, nv, ni; } f;
    f.id = t->id; wire_state(&f.st, s); f.prim = (uint32_t)prim; f.nv = (uint32_t)nv; f.ni = idx ? (uint32_t)ni : 0;
    seg_t b[2] = { { v, (uint32_t)(sizeof(rvert_t) * nv), 0, 0 }, { idx, f.ni * 2, 0, 0 } };
    br_send(BR_DRAW, &f, sizeof f, b, f.ni ? 2 : 1);
}
static void r_blit(rtarget_t *t, int dx, int dy, int dw, int dh, const uint32_t *argb, int sw, int sh, int pitch, int keyed) {
    if (sw <= 0 || sh <= 0) return;
    uint32_t f[8] = { t->id, (uint32_t)dx, (uint32_t)dy, (uint32_t)dw, (uint32_t)dh, (uint32_t)sw, (uint32_t)sh, (uint32_t)keyed };
    seg_t b = { argb, (uint32_t)sw * 4, (uint32_t)sh, (uint32_t)pitch * 4 };
    br_send(BR_BLIT, f, sizeof f, &b, 1);
}
static void r_copy(rtarget_t *t, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
    uint32_t f[9] = { t->id, (uint32_t)dx, (uint32_t)dy, (uint32_t)dw, (uint32_t)dh, (uint32_t)sx, (uint32_t)sy, (uint32_t)sw, (uint32_t)sh };
    br_send(BR_COPY, f, sizeof f, NULL, 0);
}
static void r_copy_from(rtarget_t *t, int dx, int dy, int dw, int dh, rtarget_t *src, int sx, int sy, int sw, int sh) {
    uint32_t f[10] = { t->id, (uint32_t)dx, (uint32_t)dy, (uint32_t)dw, (uint32_t)dh, src->id, (uint32_t)sx, (uint32_t)sy, (uint32_t)sw, (uint32_t)sh };
    br_send(BR_COPYFROM, f, sizeof f, NULL, 0);
}
/* keep at most two frames in flight */
static void pace(uint32_t frame) {
    host_kick();
    while ((int32_t)(frame - A_LOAD(&g_br->frames_done)) > 2) {
        uint32_t v = A_LOAD(&g_br->frames_done);
        A_STORE(&g_br->guest_waiting, 1);
        if ((int32_t)(frame - v) > 2) br_futex_wait(&g_br->frames_done, v, 50);
        A_STORE(&g_br->guest_waiting, 0);
        if (A_LOAD(&g_br->quit) > 1) _exit(0);
    }
}
static void r_present(rtarget_t *t, int w, int h) {
    uint32_t fr = __atomic_add_fetch(&g_frame, 1, __ATOMIC_SEQ_CST);
    uint32_t f[4] = { t->id, (uint32_t)w, (uint32_t)h, fr };
    br_send(BR_PRESENT, f, sizeof f, NULL, 0);
    pace(fr);
}
static void r_present_argb(const uint32_t *argb, int w, int h, int pitch) {
    uint32_t fr = __atomic_add_fetch(&g_frame, 1, __ATOMIC_SEQ_CST);
    uint32_t f[3] = { (uint32_t)w, (uint32_t)h, fr };
    seg_t b = { argb, (uint32_t)w * 4, (uint32_t)h, (uint32_t)pitch * 4 };
    br_send(BR_PRESENTA, f, sizeof f, &b, 1);
    pace(fr);
}
static rbackend_t g_remote = { "remote-gl", r_tex_create, r_tex_upload, r_tex_destroy, r_target_create, r_target_destroy,
                               r_target_upload, r_target_download, r_clear, r_draw, r_blit, r_copy, r_present, r_present_argb, r_copy_from };
static rbackend_t *remote_backend(void) { return &g_remote; }

/* ------------------------------------------------------------------ */
/* platform interface                                                   */
extern rbackend_t *(*g_platform_backend)(void);
extern void win_post_message(HWND h, UINT msg, WPARAM w, LPARAM l);
extern void win_input_key(int vk, int scancode, int down);
extern void win_input_char(int ch);
extern void win_input_mouse(int x, int y, int button, int down);
extern HWND g_main_hwnd;

void video_init(void) {
    const char *fds = getenv("GP3_BRIDGE_FD");
    if (!fds) rt_fatal("gp3-game must be started by the gp3 launcher");
    int fd = atoi(fds);
    void *m = mmap(NULL, BR_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) rt_fatal("bridge: mmap failed");
    g_br = m;
    if (g_br->magic != BR_MAGIC || g_br->version != BR_VERSION) rt_fatal("bridge: launcher version mismatch");
    g_ring = (uint8_t *)m + g_br->ring_off; g_reply = (uint8_t *)m + g_br->reply_off;
    g_platform_backend = remote_backend;
    { extern int g_text_from_host; g_text_from_host = 1; }
    LOG("bridge connected (%u MB shared)", BR_SHM_SIZE >> 20);
}
void script_start(void) {}

void plat_pump(void) {
    static uint32_t quit_t;
    if (!g_br) return;
    uint32_t tail = g_br->ev_tail;
    while (tail != A_LOAD(&g_br->ev_head)) {
        br_event_t e = g_br->ev[tail % BR_NEV];
        tail++;
        switch (e.type) {
        case BE_KEY: win_input_key(e.a, e.b, e.c); break;
        case BE_CHAR: win_input_char(e.a); break;
        case BE_MOUSE: win_input_mouse(e.a, e.b, e.c, e.d); break;
        case BE_MOVE: win_input_mouse(e.a, e.b, 0, 0); break;
        case BE_ACTIVATE: if (g_main_hwnd) win_post_message(g_main_hwnd, 6 /*WM_ACTIVATE*/, e.a ? 1 : 0, 0); break;
        case BE_QUIT: if (g_main_hwnd) win_post_message(g_main_hwnd, 0x10 /*WM_CLOSE*/, 0, 0); quit_t = rt_ticks_ms(); break;
        }
    }
    A_STORE(&g_br->ev_tail, tail);
    if (quit_t && rt_ticks_ms() - quit_t > 4000) { LOG("host asked to quit; exiting"); if (g_log) fflush(g_log); _exit(0); }
    if (A_LOAD(&g_br->quit) > 1) _exit(0);
}
void plat_present(const uint32_t *xrgb, int w, int h, int pitch_px) { r_present_argb(xrgb, w, h, pitch_px); }
void plat_client_size(int *w, int *h) { *w = g_mode_w; *h = g_mode_h; }
void plat_set_mode(int w, int h, int bpp) {
    g_mode_w = w; g_mode_h = h;
    uint32_t f[3] = { (uint32_t)w, (uint32_t)h, (uint32_t)bpp };
    br_send(BR_SETMODE, f, sizeof f, NULL, 0); flush_kick();
    LOG("display mode -> %dx%dx%d", w, h, bpp);
}
void plat_show_cursor(int show) { uint32_t f[1] = { (uint32_t)show }; br_send(BR_CURSOR, f, 4, NULL, 0); }
void plat_message_box(const char *cap, const char *text) {
    size_t a = strlen(cap ? cap : "") + 1, b = strlen(text ? text : "") + 1;
    char *buf = malloc(a + b); memcpy(buf, cap ? cap : "", a); memcpy(buf + a, text ? text : "", b);
    uint32_t rl; br_request(BR_MSGBOX, buf, (uint32_t)(a + b), &rl);   /* blocks until dismissed */
    free(buf);
}
void plat_screenshot(const char *name) { br_send(BR_SHOT, name, (uint32_t)strlen(name) + 1, NULL, 0); }
void plat_set_window(void *hwnd, unsigned flags) { (void)hwnd; (void)flags; }

/* ------------------------------------------------------------------ */
/* audio sink: stereo s16 into the shared ring, ~45 ms ahead of the host */
#define AUDIO_TARGET_FRAMES 2048
int plat_audio_rate(void) { return g_br ? (int)g_br->a_rate : 0; }
uint32_t plat_audio_space(void) {
    uint32_t queued = (g_br->a_head - A_LOAD(&g_br->a_tail)) / 4;
    return queued >= AUDIO_TARGET_FRAMES ? 0 : AUDIO_TARGET_FRAMES - queued;
}
void plat_audio_write(const int16_t *lr, uint32_t frames) {
    uint8_t *a = (uint8_t *)g_br + g_br->audio_off;
    uint32_t h = g_br->a_head, n = frames * 4;
    const uint8_t *s = (const uint8_t *)lr;
    for (uint32_t i = 0; i < n; i++) a[(h + i) & (BR_AUDIO_SIZE - 1)] = s[i];
    A_STORE(&g_br->a_head, h + n);
}

/* force feedback commands go to the host (SDL_Haptic) */
void plat_ff(const br_ff_t *f) { if (g_br) { br_send(BR_FF, f, sizeof *f, NULL, 0); host_kick(); } }
