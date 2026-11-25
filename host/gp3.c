/* gp3 - Linux launcher/host for Grand Prix 3.
 *
 * Owns the window (SDL2), the OpenGL renderer (rdr_gl.c), audio output and
 * input devices, and runs the game itself in a separate 32-bit process
 * (gp3-game, a static binary that loads Gp3.exe). The two talk through
 * shared memory (src/bridge.h). Nothing from Wine or Steam is involved;
 * the only runtime dependency is libSDL2 + the system OpenGL driver.
 *
 * usage: gp3 [game-dir]       (default: current directory, then the
 *                              directory of this program) */
#include <SDL.h>
#include "../src/gameexe.h"
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include "../src/bridge.h"
#include "../src/rdr.h"
#include "../src/rdr_gl.h"
#include "keys.h"

#ifdef GPXP_MAIN
#define VERSION "1.2.1"      /* GPxPatch for Linux */
#else
#define VERSION "1.2.1"
#endif
/* ------------------------------------------------------------------ */
static FILE *g_logf;
void hlog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    FILE *o = g_logf ? g_logf : stderr;
    fprintf(o, "[host %6ld.%03ld] ", (long)ts.tv_sec % 100000, ts.tv_nsec / 1000000);
    vfprintf(o, fmt, ap); fputc('\n', o); fflush(o);
    va_end(ap);
}
static void fatal_box(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    hlog("FATAL: %s", b);
    fprintf(stderr, "gp3: %s\n", b);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Grand Prix 3", b, NULL);
    exit(1);
}
/* stubs used by rdr_gl.c */
uint32_t g_bd[16];

/* ------------------------------------------------------------------ */
/* config (gp3rt.ini, same file as the Windows version)                 */
static char g_ini[PATH_MAX];
static int ini_int(const char *sec, const char *key, int def) {
    FILE *f = fopen(g_ini, "r"); if (!f) return def;
    char line[512], cur[64] = ""; int v = def;
    while (fgets(line, sizeof line, f)) {
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (*s == ';' || *s == '#') continue;
        if (*s == '[') { char *e = strchr(s, ']'); if (e) { *e = 0; snprintf(cur, sizeof cur, "%s", s + 1); } continue; }
        char *eq = strchr(s, '='); if (!eq || strcasecmp(cur, sec)) continue;
        *eq = 0; char *k = s, *e = eq; while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (strcasecmp(k, key)) continue;
        char *val = eq + 1; while (*val == ' ') val++;
        if (!strncasecmp(val, "auto", 4)) v = -1;
        else if (!strncasecmp(val, "fullscreen", 10) || !strncasecmp(val, "keep", 4) || !strncasecmp(val, "on", 2) || !strncasecmp(val, "true", 4)) v = 1;
        else if (!strncasecmp(val, "window", 6) || !strncasecmp(val, "stretch", 7) || !strncasecmp(val, "off", 3) || !strncasecmp(val, "false", 5)) v = 0;
        else v = (int)strtol(val, NULL, 0);
    }
    fclose(f); return v;
}

/* ------------------------------------------------------------------ */
/* shared memory                                                        */
static br_shared_t *B;
static uint8_t *g_ring, *g_reply, *g_audio;
#define A_LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define A_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define A_ADD(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
static int shm_create(void) {
    int fd = (int)syscall(SYS_memfd_create, "gp3-bridge", 0);
    if (fd < 0) { char p[64]; snprintf(p, sizeof p, "/dev/shm/gp3-%d", getpid()); fd = open(p, O_RDWR | O_CREAT | O_EXCL, 0600); unlink(p); }
    if (fd < 0 || ftruncate(fd, BR_SHM_SIZE)) fatal_box("cannot create shared memory");
    void *m = mmap(NULL, BR_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) fatal_box("cannot map shared memory");
    B = m; memset(B, 0, sizeof *B);
    B->magic = BR_MAGIC; B->version = BR_VERSION; B->total_size = BR_SHM_SIZE;
    B->ring_off = 4096 + (((uint32_t)sizeof(br_shared_t) + 4095) & ~4095u);
    B->reply_off = B->ring_off + BR_RING_SIZE;
    B->audio_off = B->reply_off + BR_REPLY_SIZE;
    if (B->audio_off + BR_AUDIO_SIZE > BR_SHM_SIZE) fatal_box("bridge layout error");
    g_ring = (uint8_t *)m + B->ring_off; g_reply = (uint8_t *)m + B->reply_off; g_audio = (uint8_t *)m + B->audio_off;
    return fd;
}
static void push_event(uint32_t type, int a, int b, int c, int d) {
    uint32_t h = B->ev_head;
    if (h - A_LOAD(&B->ev_tail) >= BR_NEV) return;   /* full: drop */
    br_event_t *e = &B->ev[h % BR_NEV]; e->type = type; e->a = a; e->b = b; e->c = c; e->d = d;
    A_STORE(&B->ev_head, h + 1);
}

/* ------------------------------------------------------------------ */
/* window + GL                                                          */
static SDL_Window *g_win;
static SDL_GLContext g_glc;
static rbackend_t *R;
static int g_fullscreen = 1, g_pres_w = 640, g_pres_h = 480, g_mode_w = 640, g_mode_h = 480;
static char g_game_dir[PATH_MAX], g_shot_dir[PATH_MAX + 16];
static int g_shot_pending; static char g_shot_name[128];
extern int rdr_gl_read_window(uint32_t *argb, int w, int h);
extern int write_png(const char *path, const uint32_t *xrgb, int w, int h, int pitch_px);

static void *gl_getproc(const char *n) { return SDL_GL_GetProcAddress(n); }
static void win_size(void *c, int *w, int *h) { (void)c; SDL_GL_GetDrawableSize(g_win, w, h); }
static void take_shot(void) {
    int w, h; SDL_GL_GetDrawableSize(g_win, &w, &h);
    uint32_t *px = malloc((size_t)w * h * 4); if (!px) return;
    rdr_gl_read_window(px, w, h);
    mkdir(g_shot_dir, 0755);
    char p[PATH_MAX + 200];
    if (g_shot_name[0]) snprintf(p, sizeof p, "%s/%s.png", g_shot_dir, g_shot_name);
    else for (int i = 0; i < 100000; i++) { snprintf(p, sizeof p, "%s/gp3linux_%05d.png", g_shot_dir, i); if (access(p, F_OK)) break; }
    write_png(p, px, w, h, w);
    hlog("screenshot %s", p);
    free(px); g_shot_pending = 0; g_shot_name[0] = 0;
}
static void gl_swap(void *c) {
    (void)c;
    if (g_shot_pending) take_shot();
    SDL_GL_SwapWindow(g_win);
}
static void glog_cb(const char *m) { hlog("%s", m); }

/* window pixel -> game pixel (the game image is letterboxed like rdr_gl's present) */
static void map_mouse(int x, int y, int *gx, int *gy) {
    int ww, wh, dw, dh; SDL_GetWindowSize(g_win, &ww, &wh); SDL_GL_GetDrawableSize(g_win, &dw, &dh);
    double sx = ww ? (double)dw / ww : 1, sy = wh ? (double)dh / wh : 1;
    double px = x * sx, py = y * sy;
    double sa = (double)g_pres_w / g_pres_h, wa = (double)dw / dh;
    double vw = dw, vh = dh; if (wa > sa) vw = dh * sa; else vh = dw / sa;
    double vx = (dw - vw) / 2, vy = (dh - vh) / 2;
    int X = (int)((px - vx) * g_mode_w / vw), Y = (int)((py - vy) * g_mode_h / vh);
    if (X < 0) X = 0; if (Y < 0) Y = 0; if (X >= g_mode_w) X = g_mode_w - 1; if (Y >= g_mode_h) Y = g_mode_h - 1;
    *gx = X; *gy = Y;
}

/* ------------------------------------------------------------------ */
/* object tables (ids are allocated by the game process)                */
typedef struct { uint32_t id; void *p; int w, h; } obj_t;
static obj_t *g_objs; static uint32_t g_ocap;
static obj_t *obj_slot(uint32_t id, int create) {
    if (!g_ocap) { g_ocap = 1 << 16; g_objs = calloc(g_ocap, sizeof *g_objs); }
    uint32_t m = g_ocap - 1, i = (id * 2654435761u) & m;
    for (;;) {
        if (g_objs[i].id == id) return &g_objs[i];
        if (!g_objs[i].id) { if (!create) return NULL; g_objs[i].id = id; return &g_objs[i]; }
        i = (i + 1) & m;
    }
}
static uint32_t g_ocount;
static void obj_put(uint32_t id, void *p, int w, int h) {
    if (++g_ocount * 2 > g_ocap) {   /* grow (deleted slots keep id with p = NULL; they are dropped here) */
        obj_t *old = g_objs; uint32_t oc = g_ocap;
        g_ocap *= 2; g_objs = calloc(g_ocap, sizeof *g_objs); g_ocount = 1;
        for (uint32_t i = 0; i < oc; i++) if (old[i].id && old[i].p) { obj_t *s = obj_slot(old[i].id, 1); *s = old[i]; g_ocount++; }
        free(old);
    }
    obj_t *s = obj_slot(id, 1); s->p = p; s->w = w; s->h = h;
}
static obj_t *obj_get(uint32_t id) { obj_t *s = id ? obj_slot(id, 0) : NULL; return s && s->p ? s : NULL; }
static void *tex_of(uint32_t id) { obj_t *o = obj_get(id); return o ? o->p : NULL; }

static uint8_t *g_stage; static size_t g_stage_n, g_stage_cap;

static void unwire_state(rstate_t *s, const br_state_t *w) {
    memset(s, 0, sizeof *s);
#define C(n) s->n = w->n;
    C(zenable) C(zwrite) C(zfunc) C(cull) C(blend) C(srcblend) C(dstblend) C(atest) C(afunc) C(aref) C(fog) C(fogtable) C(fogcolor)
    C(fogstart) C(fogend) C(fogdensity) C(flat) C(specular) C(ckey) C(dither) C(tfactor) C(zbias) C(nstages) C(vx) C(vy) C(vw) C(vh) C(minz) C(maxz)
#undef C
    for (int i = 0; i < 2; i++) {
        const br_stage_t *a = &w->st[i]; rstage_t *b = &s->st[i];
        b->tex = a->tex ? (rtex_t *)tex_of(a->tex) : NULL;
#define C(n) b->n = a->n;
        C(has_alpha) C(has_ckey) C(colorop) C(carg1) C(carg2) C(alphaop) C(aarg1) C(aarg2) C(texcoord) C(addru) C(addrv) C(magf) C(minf) C(mipf) C(border) C(lodbias)
#undef C
    }
}
static void reply_done(uint32_t len) {
    B->reply_len = len;
    A_ADD(&B->reply_seq, 1);
    br_futex_wake(&B->reply_seq);
}
static uint64_t g_frames, g_draws;

/* execute one command; returns 1 after a present */
static int exec_cmd(uint32_t op, const uint8_t *p, uint32_t len) {
    int staged = (op & BR_STAGED) != 0; op &= ~BR_STAGED;
    const uint32_t *u = (const uint32_t *)p;
#define BULK(fixed) (staged ? g_stage : p + (fixed))
    int presented = 0;
    switch (op) {
    case BR_CHUNK:
        if (g_stage_n + len > g_stage_cap) { g_stage_cap = (g_stage_n + len) * 2; g_stage = realloc(g_stage, g_stage_cap); }
        memcpy(g_stage + g_stage_n, p, len); g_stage_n += len; return 0;
    case BR_TEXC: { void *t = R->tex_create((int)u[1], (int)u[2], (int)u[3]); obj_put(u[0], t, (int)u[1], (int)u[2]); break; }
    case BR_TEXU: { obj_t *o = obj_get(u[0]); if (o) R->tex_upload(o->p, (int)u[1], (int)u[2], (int)u[3], (const uint32_t *)BULK(16)); break; }
    case BR_TEXD: { obj_t *o = obj_get(u[0]); if (o) { R->tex_destroy(o->p); o->p = NULL; } break; }
    case BR_TGTC: { void *t = R->target_create((int)u[1], (int)u[2], (int)u[3]); obj_put(u[0], t, (int)u[1], (int)u[2]); break; }
    case BR_TGTD: { obj_t *o = obj_get(u[0]); if (o) { R->target_destroy(o->p); o->p = NULL; } break; }
    case BR_TGTU: { obj_t *o = obj_get(u[0]); if (!o) break;
        int x = (int)u[1], y = (int)u[2], w = (int)u[3], h = (int)u[4];
        /* target_upload expects a full-image pointer; ours holds just the rectangle */
        const uint32_t *px = (const uint32_t *)BULK(20);
        R->target_upload(o->p, px - ((size_t)y * w + x), w, x, y, w, h);
        break; }
    case BR_TGTDL: { obj_t *o = obj_get(u[0]); uint32_t y0 = u[1], n = u[2];
        if (o && (size_t)n * o->w * 4 <= BR_REPLY_SIZE) {
            uint32_t *base = (uint32_t *)g_reply - (size_t)y0 * o->w;
            R->target_download(o->p, base, o->w, (int)y0, (int)n);
            reply_done(n * (uint32_t)o->w * 4);
        } else reply_done(0);
        break; }
    case BR_CLEAR: { obj_t *o = obj_get(u[0]); uint32_t nr = u[1];
        const uint8_t *b = BULK(8); const int *rects = (const int *)b; const uint32_t *g = (const uint32_t *)(b + nr * 16);
        float z; memcpy(&z, &g[3], 4);
        if (o) R->clear(o->p, rects, (int)nr, (int)g[0], g[1], (int)g[2], z, (int)g[4], (int)g[5], (int)g[6], (int)g[7]);
        break; }
    case BR_DRAW: {
        struct { uint32_t id; br_state_t st; uint32_t prim, nv, ni; } f; memcpy(&f, p, sizeof f);
        obj_t *o = obj_get(f.id); if (!o) break;
        const uint8_t *b = BULK(sizeof f);
        rstate_t s; unwire_state(&s, &f.st);
        R->draw(o->p, &s, (int)f.prim, (const rvert_t *)b, (int)f.nv, f.ni ? (const uint16_t *)(b + sizeof(rvert_t) * f.nv) : NULL, (int)f.ni);
        g_draws++;
        break; }
    case BR_BLIT: { obj_t *o = obj_get(u[0]); if (o) R->blit(o->p, (int)u[1], (int)u[2], (int)u[3], (int)u[4], (const uint32_t *)BULK(32), (int)u[5], (int)u[6], (int)u[5], (int)u[7]); break; }
    case BR_COPY: { obj_t *o = obj_get(u[0]); if (o) R->copy(o->p, (int)u[1], (int)u[2], (int)u[3], (int)u[4], (int)u[5], (int)u[6], (int)u[7], (int)u[8]); break; }
    case BR_COPYFROM: { obj_t *o = obj_get(u[0]), *s = obj_get(u[5]); if (o && s) R->copy_from(o->p, (int)u[1], (int)u[2], (int)u[3], (int)u[4], s->p, (int)u[6], (int)u[7], (int)u[8], (int)u[9]); break; }
    case BR_PRESENT: { obj_t *o = obj_get(u[0]);
        if (o) { g_pres_w = o->w; g_pres_h = o->h; R->present(o->p, (int)u[1], (int)u[2]); }
        A_STORE(&B->frames_done, u[3]); br_futex_wake(&B->frames_done);
        g_frames++; presented = 1; break; }
    case BR_PRESENTA: { g_pres_w = (int)u[0]; g_pres_h = (int)u[1];
        R->present_argb((const uint32_t *)BULK(12), (int)u[0], (int)u[1], (int)u[0]);
        A_STORE(&B->frames_done, u[2]); br_futex_wake(&B->frames_done);
        g_frames++; presented = 1; break; }
    case BR_SETMODE: g_mode_w = (int)u[0]; g_mode_h = (int)u[1]; g_pres_w = g_mode_w; g_pres_h = g_mode_h;
        hlog("game display mode %dx%dx%d", g_mode_w, g_mode_h, (int)u[2]); break;
    case BR_CURSOR: SDL_ShowCursor(u[0] ? SDL_ENABLE : SDL_DISABLE); break;
    case BR_MSGBOX: { const char *cap = (const char *)p, *txt = cap + strlen(cap) + 1;
        hlog("message box: %s: %s", cap, txt);
        if (g_fullscreen) SDL_SetWindowFullscreen(g_win, 0);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, cap, txt, g_win);
        if (g_fullscreen) SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        reply_done(0); break; }
    case BR_SYNC: reply_done(0); break;
    case BR_TITLE: SDL_SetWindowTitle(g_win, (const char *)p); break;
    case BR_SHOT: snprintf(g_shot_name, sizeof g_shot_name, "%s", (const char *)p); g_shot_pending = 1; break;
    case BR_FF: { extern void joy_ff(const br_ff_t *f); joy_ff((const br_ff_t *)p); break; }
    default: hlog("unknown command %u (len %u)", op, len); break;
    }
    if (staged) g_stage_n = 0;
    return presented;
#undef BULK
}

/* run commands until the ring is empty or a frame was presented */
static int drain(void) {
    int any = 0;
    for (;;) {
        uint32_t tail = B->tail, head = A_LOAD(&B->head);
        if (tail == head) break;
        uint32_t off = tail & (BR_RING_SIZE - 1);
        br_hdr_t h = *(br_hdr_t *)(g_ring + off);
        int pres = 0;
        if (h.op != BR_WRAP) pres = exec_cmd(h.op, g_ring + off + 8, h.len);
        A_STORE(&B->tail, tail + 8 + h.len);
        A_ADD(&B->space_wake, 1);
        if (A_LOAD(&B->guest_waiting)) br_futex_wake(&B->space_wake);
        any = 1;
        if (pres) break;
    }
    return any;
}

/* ------------------------------------------------------------------ */
/* audio: the game mixes; we just play the ring                         */
static SDL_AudioDeviceID g_adev;
static void audio_cb(void *ud, Uint8 *out, int n) {
    (void)ud;
    uint32_t head = A_LOAD(&B->a_head), tail = B->a_tail, have = head - tail;
    if (have > BR_AUDIO_SIZE) { tail = head - BR_AUDIO_SIZE / 2; have = BR_AUDIO_SIZE / 2; }   /* guest ran away: resync */
    uint32_t k = have < (uint32_t)n ? have : (uint32_t)n;
    for (uint32_t i = 0; i < k; i++) out[i] = g_audio[(tail + i) & (BR_AUDIO_SIZE - 1)];
    if (k < (uint32_t)n) { memset(out + k, 0, n - k); if (head != tail) B->a_underruns++; }
    A_STORE(&B->a_tail, tail + k);
}
static void audio_open(void) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) { hlog("audio: %s", SDL_GetError()); return; }
    SDL_AudioSpec want = { 0 }, have;
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512; want.callback = audio_cb;
    g_adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_adev) { hlog("audio: no device (%s)", SDL_GetError()); return; }
    B->a_rate = (uint32_t)have.freq;
    hlog("audio: %s, %d Hz, %d-sample buffer", SDL_GetCurrentAudioDriver(), have.freq, have.samples);
    SDL_PauseAudioDevice(g_adev, 0);
}

/* test script (same format as the headless runner): "<ms> key <name> [down|up]",
   "<ms> type <text>", "<ms> click <x<<16|y> <0|1>", "<ms> shot <name>", "<ms> quit" */
typedef struct { uint32_t t; int kind, vk, down; char arg[96]; } sev_t;
static sev_t g_sev[1024]; static int g_nsev, g_nexts; static uint64_t g_script_t0;
static int vk_name(const char *s) {
    static const struct { const char *n; int vk; } t[] = {
        {"enter", 0x0d}, {"esc", 0x1b}, {"space", 0x20}, {"left", 0x25}, {"up", 0x26}, {"right", 0x27}, {"down", 0x28}, {"tab", 0x09},
        {"shift", 0x10}, {"ctrl", 0x11}, {"alt", 0x12}, {"backspace", 0x08}, {"pgup", 0x21}, {"pgdn", 0x22}, {"home", 0x24}, {"end", 0x23},
        {"f1", 0x70}, {"f2", 0x71}, {"f3", 0x72}, {"f4", 0x73}, {"f5", 0x74}, {"f6", 0x75}, {"f7", 0x76}, {"f8", 0x77}, {"f9", 0x78},
        {"f10", 0x79}, {"f11", 0x7a}, {"f12", 0x7b}, {NULL, 0} };
    for (int i = 0; t[i].n; i++) if (!strcasecmp(t[i].n, s)) return t[i].vk;
    if (strlen(s) == 1) return (s[0] >= 'a' && s[0] <= 'z') ? s[0] - 32 : s[0];
    return 0;
}
static int vk_scan(int vk) {
    static const struct { int vk, sc; } t[] = { {0x0d,0x1c},{0x1b,0x01},{0x20,0x39},{0x25,0x14b},{0x26,0x148},{0x27,0x14d},{0x28,0x150},{0x09,0x0f},
        {0x10,0x2a},{0x11,0x1d},{0x12,0x38},{0x08,0x0e},{0x21,0x149},{0x22,0x151},{0x24,0x147},{0x23,0x14f},{0,0} };
    for (int i = 0; t[i].vk; i++) if (t[i].vk == vk) return t[i].sc;
    if (vk >= 0x70 && vk <= 0x79) return 0x3b + vk - 0x70;
    return 0;
}
static void script_load(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { hlog("script %s not found", path); return; }
    char line[256];
    while (fgets(line, sizeof line, f) && g_nsev < 1020) {
        char ts[32], cmd[32], a1[96] = "", a2[32] = "";
        if (line[0] == '#' || sscanf(line, "%31s %31s %95s %31s", ts, cmd, a1, a2) < 2) continue;
        sev_t *e = &g_sev[g_nsev]; memset(e, 0, sizeof *e); e->t = (uint32_t)atoi(ts);
        if (!strcmp(cmd, "key") || !strcmp(cmd, "tap")) {
            e->kind = 1; e->vk = vk_name(a1);
            if (!strcmp(a2, "down")) e->down = 1; else if (!strcmp(a2, "up")) e->down = 0;
            else { e->down = 1; g_nsev++; g_sev[g_nsev] = *e; g_sev[g_nsev].down = 0; g_sev[g_nsev].t += 80; }
        } else if (!strcmp(cmd, "type")) { e->kind = 2; snprintf(e->arg, sizeof e->arg, "%s", a1); }
        else if (!strcmp(cmd, "click")) { e->kind = 3; e->vk = atoi(a1); e->down = atoi(a2); }
        else if (!strcmp(cmd, "shot")) { e->kind = 4; snprintf(e->arg, sizeof e->arg, "%s", a1); }
        else if (!strcmp(cmd, "wclick")) {   /* window pixel coordinates: press, release 120 ms later */
            e->kind = 6; e->vk = atoi(a1); e->down = atoi(a2);
            g_nsev++; g_sev[g_nsev] = *e; g_sev[g_nsev].kind = 7; g_sev[g_nsev].t += 120;
        }
        else if (!strcmp(cmd, "quit")) e->kind = 5;
        else continue;
        g_nsev++;
    }
    fclose(f);
    for (int i = 1; i < g_nsev; i++) { sev_t k = g_sev[i]; int j = i - 1; while (j >= 0 && g_sev[j].t > k.t) { g_sev[j + 1] = g_sev[j]; j--; } g_sev[j + 1] = k; }
    hlog("script: %d events", g_nsev);
}
static int script_run(void) {   /* returns 1 on quit */
    uint64_t now = (uint64_t)SDL_GetTicks() - g_script_t0;
    while (g_nexts < g_nsev && g_sev[g_nexts].t <= now) {
        sev_t *e = &g_sev[g_nexts++];
        switch (e->kind) {
        case 1: push_event(BE_KEY, e->vk, vk_scan(e->vk), e->down, 0); break;
        case 2: for (char *c = e->arg; *c; c++) push_event(BE_CHAR, *c == '_' ? ' ' : *c, 0, 0, 0); break;
        case 3: push_event(BE_MOUSE, e->vk >> 16, e->vk & 0xffff, 1, e->down); break;
        case 4: snprintf(g_shot_name, sizeof g_shot_name, "%s", e->arg); g_shot_pending = 1; break;
        case 5: return 1;
        case 6: {   /* synthesize real SDL mouse events (tests the window -> game mapping) */
            SDL_Event m; memset(&m, 0, sizeof m);
            m.type = SDL_MOUSEMOTION; m.motion.x = e->vk; m.motion.y = e->down; SDL_PushEvent(&m);
            memset(&m, 0, sizeof m); m.type = SDL_MOUSEBUTTONDOWN; m.button.button = SDL_BUTTON_LEFT; m.button.x = e->vk; m.button.y = e->down; SDL_PushEvent(&m);
            break; }
        case 7: { SDL_Event m; memset(&m, 0, sizeof m); m.type = SDL_MOUSEBUTTONUP; m.button.button = SDL_BUTTON_LEFT; m.button.x = e->vk; m.button.y = e->down; SDL_PushEvent(&m); break; }
        }
    }
    return 0;
}

/* joysticks / force feedback: host/joy.c */
void joy_init(br_shared_t *b); void joy_event(const SDL_Event *e); void joy_poll(void); void joy_shutdown(void);

/* ------------------------------------------------------------------ */
/* NumLock: SDL only learns its state from NumLock key presses, so a NumLock switched on at
   boot looks "off" to the game (the keypad then gives the wrong keys until NumLock is pressed
   twice). Read the real LED from the X server at start and whenever the window gets focus. */
static void sync_numlock(void) {
    const char *drv = SDL_GetCurrentVideoDriver(); if (!drv || strcmp(drv, "x11")) return;
    typedef void *(*open_t)(const char *); typedef unsigned long (*atom_t)(void *, const char *, int);
    typedef int (*named_t)(void *, unsigned long, int *, int *, void *, int *);
    static open_t xopen; static atom_t xatom; static named_t xnamed; static void *dpy; static unsigned long a_num; static int tried;
    if (!tried) {
        tried = 1; void *h = SDL_LoadObject("libX11.so.6"); if (!h) return;
        xopen = (open_t)SDL_LoadFunction(h, "XOpenDisplay"); xatom = (atom_t)SDL_LoadFunction(h, "XInternAtom"); xnamed = (named_t)SDL_LoadFunction(h, "XkbGetNamedIndicator");
        if (xopen && xatom && xnamed && (dpy = xopen(NULL))) a_num = xatom(dpy, "Num Lock", 0);
    }
    if (!dpy || !a_num) return;
    int ndx = 0, on = 0, real = 0;
    if (!xnamed(dpy, a_num, &ndx, &on, NULL, &real)) return;
    SDL_Keymod m = SDL_GetModState(); int cur = (m & KMOD_NUM) != 0;
    if (!!on != cur) { SDL_SetModState((SDL_Keymod)(on ? (m | KMOD_NUM) : (m & ~KMOD_NUM))); hlog("NumLock is %s (synchronised with the keyboard LED)", on ? "on" : "off"); }
}

static pid_t g_child;
static void spawn_game(int fd, char **extra, int nextra) {
    char self[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) fatal_box("cannot locate myself");
    self[n] = 0; char *sl = strrchr(self, '/'); if (sl) *sl = 0;
    char game[PATH_MAX + 32]; snprintf(game, sizeof game, "%s/gp3-game", self);
    if (access(game, X_OK)) fatal_box("gp3-game not found next to gp3 (%s)", game);
    char logp[PATH_MAX + 32]; snprintf(logp, sizeof logp, "%s/gp3linux-game.log", g_game_dir);
    char fds[16]; snprintf(fds, sizeof fds, "%d", fd);
    char *argv[64]; int a = 0;
    argv[a++] = game; argv[a++] = "--log"; argv[a++] = logp;
    for (int i = 0; i < nextra && a < 60; i++) argv[a++] = extra[i];
    argv[a++] = g_game_dir; argv[a] = NULL;
    g_child = fork();
    if (g_child < 0) fatal_box("fork failed");
    if (g_child == 0) {
        int fl = fcntl(fd, F_GETFD); fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC);
        setenv("GP3_BRIDGE_FD", fds, 1);
        execv(game, argv);
        _exit(127);
    }
    hlog("game process %d started", (int)g_child);
}

static int find_game_dir(const char *arg) {
    char cand[3][PATH_MAX]; int n = 0;
    if (arg) snprintf(cand[n++], PATH_MAX, "%s", arg);
    else {
        if (getcwd(cand[n], PATH_MAX)) n++;
        ssize_t k = readlink("/proc/self/exe", cand[n], PATH_MAX - 1);
        if (k > 0) { cand[n][k] = 0; char *s = strrchr(cand[n], '/'); if (s) *s = 0; n++;
                     snprintf(cand[n], PATH_MAX, "%s/..", cand[n - 1]); n++; }
    }
    for (int i = 0; i < n; i++) {
        char t[PATH_MAX + 16];
        char en[256]; (void)t;
        if (gameexe_find(cand[i], en, sizeof en, NULL) && realpath(cand[i], g_game_dir)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* hybrid graphics (notebooks with Intel/AMD + NVIDIA/AMD): ask for the dedicated GPU
   through PRIME render offload. Must happen before libGL is loaded (before SDL_Init). */
static int g_prime;
static void prime_setup(void) {
    char b[32] = "auto";
    { FILE *f = fopen(g_ini, "r"); if (f) { char line[256]; int in = 0; while (fgets(line, sizeof line, f)) {
        if (line[0] == '[') in = !strncasecmp(line, "[video]", 7);
        else if (in && !strncasecmp(line, "gpu", 3)) { char *e = strchr(line, '='); if (e) { e++; while (*e == ' ') e++; snprintf(b, sizeof b, "%s", e); } } }
        fclose(f); } }
    if (!strncasecmp(b, "integrated", 10) || !strncasecmp(b, "system", 6) || getenv("GP3_NO_PRIME")) return;
    if (getenv("__NV_PRIME_RENDER_OFFLOAD") || getenv("DRI_PRIME")) return;   /* the user already chose */
    int nv = 0, other = 0, cards = 0;
    for (int i = 0; i < 8; i++) {
        char p[96]; snprintf(p, sizeof p, "/sys/class/drm/card%d/device/vendor", i);
        FILE *f = fopen(p, "r"); if (!f) continue;
        unsigned v = 0; if (fscanf(f, "%x", &v) == 1) { cards++; if (v == 0x10de) nv = 1; else other = 1; }
        fclose(f);
    }
    if (cards < 2 || !other) return;   /* single GPU: nothing to choose */
    if (nv && !access("/proc/driver/nvidia/version", F_OK)) {
        setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1); setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1); setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 1);
        g_prime = 1;
    } else { setenv("DRI_PRIME", "1", 1); g_prime = 2; }
}
static void prime_retry_without(char **argv) {   /* the offload setup failed: start again on the default GPU */
    hlog("dedicated GPU (PRIME) could not be used: restarting on the default GPU");
    unsetenv("__NV_PRIME_RENDER_OFFLOAD"); unsetenv("__GLX_VENDOR_LIBRARY_NAME"); unsetenv("__VK_LAYER_NV_optimus"); unsetenv("DRI_PRIME");
    setenv("GP3_NO_PRIME", "1", 1);
    char self[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) { self[n] = 0; execv(self, argv); }
}
static int is_dedicated_renderer(const char *r, const char *v) {
    if (!r) return 0;
    if (strstr(r, "llvmpipe") || strstr(r, "softpipe") || strstr(r, "Intel")) return 0;
    if (v && strstr(v, "NVIDIA")) return 1;
    if (strstr(r, "NVIDIA") || strstr(r, "GeForce") || strstr(r, "NV1") || strstr(r, "Quadro")) return 1;   /* nouveau names */
    if (strstr(r, "Radeon") && !strstr(r, "Radeon Graphics") && !strstr(r, "Radeon(TM) Graphics") && !strstr(r, "Vega 8") && !strstr(r, "Vega 6")) return 1;
    return 0;
}

#ifdef GP3_NO_GPXUI
/* first start: a "Grand Prix 3" entry in the applications menu (once; the user may delete it) */
static void first_run_shortcut(void) {
    const char *h = getenv("HOME"); if (!h || !*h) return;
    int is2000 = 0; { DIR *d = opendir(g_game_dir); struct dirent *e;
        while (d && (e = readdir(d))) if (!strcasecmp(e->d_name, "gp3 2000.exe")) is2000 = 1; if (d) closedir(d); }
    const char *gid = is2000 ? "grand-prix-3-2000" : "grand-prix-3", *gname = is2000 ? "Grand Prix 3 2000" : "Grand Prix 3";
    char mark[PATH_MAX], p[PATH_MAX], self[PATH_MAX];
    snprintf(mark, sizeof mark, "%s/.config/gp3reborn-%s.shortcut", h, gid);
    if (access(mark, F_OK) == 0) return;
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1); if (n <= 0) return; self[n] = 0;
    extern int install_icons(void); install_icons();
    snprintf(p, sizeof p, "%s/.local", h); mkdir(p, 0755); snprintf(p, sizeof p, "%s/.local/share", h); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/.local/share/applications", h); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/.local/share/applications/%s.desktop", h, gid);
    FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nComment=Grand Prix 3 (GP3RT para Linux)\nExec=\"%s\" \"%s\"\nPath=%s\n"
               "Icon=%s/.local/share/icons/hicolor/256x256/apps/gp3rt.png\nTerminal=false\nCategories=Game;Simulation;\nStartupWMClass=gp3\n", gname, self, g_game_dir, g_game_dir, h);
    fclose(f);
    snprintf(p, sizeof p, "%s/.config", h); mkdir(p, 0755);
    f = fopen(mark, "w"); if (f) fclose(f);
    hlog("menu shortcut \"%s\" created", gname);
}
#endif
int main(int argc, char **argv) {
    const char *dir = NULL; int max_seconds = 0; const char *script = NULL; int launcher = 0;
#ifdef GPXP_MAIN
    launcher = 1;     /* the separate "GPxPatch for Linux" program: only the configuration screen */
#elif !defined(GP3_NO_GPXUI)
    {   /* started as "GPxPatch" (the link next to gp3, for a double click in the file manager) */
        const char *b = strrchr(argv[0], '/'); b = b ? b + 1 : argv[0];
        for (const char *q = b; *q; q++) if (!strncasecmp(q, "gpxpatch", 8)) launcher = 1;
    }
#endif
    for (int i = 1; i < argc; i++) {
#ifdef GPXP_MAIN
        if (!strcmp(argv[i], "--version")) { printf("GPxPatch for Linux %s (screen for GPxPatch 4.52 by Rene Smit)\n", VERSION); return 0; }
#else
        if (!strcmp(argv[i], "--version")) { printf("gp3 (GP3RT for Linux) %s\n", VERSION); return 0; }
#endif
        else if (!strcmp(argv[i], "--install-icons")) { extern int install_icons(void); return install_icons() ? 0 : 1; }
        else if (!strcmp(argv[i], "--extract-icon") && i + 2 < argc) { extern int extract_icon(const char *, const char *); return extract_icon(argv[i + 1], argv[i + 2]) ? 0 : 1; }
        else if (!strcmp(argv[i], "--find-game")) { if (!find_game_dir(i + 1 < argc && argv[i + 1][0] ? argv[i + 1] : NULL)) return 1; printf("%s\n", g_game_dir); return 0; }
        else if (!strcmp(argv[i], "--max-seconds") && i + 1 < argc) max_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--gpxpatch") || !strcmp(argv[i], "--config")) launcher = 1;
        else if (!strcmp(argv[i], "--gpxpatch-web")) launcher = 2;
        else if (argv[i][0] != '-') dir = argv[i];
    }
    signal(SIGPIPE, SIG_IGN);
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_APP_NAME, "Grand Prix 3");
    if (!find_game_dir(dir)) fatal_box("Executavel do jogo (Gp3.exe ou gp3 2000.exe) nao encontrado. Rode o gp3 dentro da pasta do jogo ou passe a pasta: ./gp3 /caminho/do/GP3\n\n"
                                       "Game executable (Gp3.exe or gp3 2000.exe) not found. Run gp3 inside the game folder or pass it: ./gp3 /path/to/GP3");
#ifdef GP3_NO_GPXUI
    if (launcher) {   /* the screen is the separate "GPxPatch for Linux" program: start it if it is here */
        char self[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n > 0) { self[n] = 0; char *sl = strrchr(self, '/'); if (sl) { strcpy(sl + 1, "GPxPatch");
            if (access(self, X_OK) == 0) { execl(self, self, g_game_dir, (char *)NULL); } } }
        fatal_box("A tela do GPxPatch e um programa separado: \"GPxPatch para Linux\". Copie o pacote dele para a pasta do jogo.\n\n"
                  "The GPxPatch screen is a separate program: \"GPxPatch for Linux\". Copy its package into the game folder.");
    }
#else
    if (launcher) {   /* GPxPatch configuration screen (host/launcher.c) */
        char lp[PATH_MAX + 32]; snprintf(lp, sizeof lp, "%s/gp3linux-gpxpatch.log", g_game_dir); g_logf = fopen(lp, "w");
        extern int launcher_main(const char *dir, const char *version);
        extern int gpxui_main(const char *dir, const char *version);
#if defined(GPXP_MAIN) && defined(GPXP_CLASSIC)
        if (launcher == 1 && !getenv("GP3_GPXPATCH_WEB") && !getenv("GP3_GPXPATCH_MODERN")) {   /* René Smit's own screen (host/gpxclassic.c) */
            extern int gpxclassic_main(const char *dir, const char *version);
            int r = gpxclassic_main(g_game_dir, VERSION);
            if (r >= 0) return r;
            hlog("launcher: classic GPxPatch screen unavailable");
        }
#endif
        if (launcher == 1 && !getenv("GP3_GPXPATCH_WEB")) {   /* native window (host/gpxui.c); the browser page if it cannot run */
            int r = gpxui_main(g_game_dir, VERSION);
            if (r >= 0) return r;
            hlog("launcher: native screen unavailable, using the browser page");
        }
        return launcher_main(g_game_dir, VERSION);
    }
#endif
#ifdef GPXP_MAIN
    return 0;
#endif
    char lp[PATH_MAX + 32]; snprintf(lp, sizeof lp, "%s/gp3linux.log", g_game_dir); g_logf = fopen(lp, "w");
#ifdef GP3_NO_GPXUI
    first_run_shortcut();
#endif
    snprintf(g_ini, sizeof g_ini, "%s/gp3rt.ini", g_game_dir);
    snprintf(g_shot_dir, sizeof g_shot_dir, "%s/screens", g_game_dir);
    hlog("GP3RT for Linux %s - game folder %s", VERSION, g_game_dir);
    prime_setup();
    if (g_prime == 1) hlog("hybrid graphics: requesting the NVIDIA GPU (PRIME render offload)");
    else if (g_prime == 2) hlog("hybrid graphics: requesting the dedicated GPU (DRI_PRIME=1)");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) fatal_box("SDL: %s", SDL_GetError());

    g_fullscreen = ini_int("video", "mode", 1);
    int vsync = ini_int("video", "vsync", 1), scale = ini_int("video", "internal_scale", -1);   /* -1/0: auto */
    int aniso = ini_int("video", "anisotropy", -1), filt = ini_int("video", "force_filtering", 1), wscale = ini_int("video", "window_scale", 1);
    if (scale > 8) scale = 8; if (wscale < 1) wscale = 1;

    int fd = shm_create();
    B->opt_scale = scale; B->opt_vsync = vsync;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    Uint32 wf = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (g_fullscreen) wf |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    int cw = 640 * wscale, ch = 480 * wscale;
    SDL_DisplayMode dm;
    if (g_fullscreen && !SDL_GetDesktopDisplayMode(0, &dm)) { cw = dm.w; ch = dm.h; }   /* also right without a window manager */
    g_win = SDL_CreateWindow("Grand Prix 3", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, cw, ch, wf);
    if (!g_win && g_prime) prime_retry_without(argv);
    if (!g_win) fatal_box("nao foi possivel criar a janela: %s", SDL_GetError());
    { extern void set_window_icon(SDL_Window *); set_window_icon(g_win); }
    sync_numlock();
    g_glc = SDL_GL_CreateContext(g_win);
    if (!g_glc && g_prime) prime_retry_without(argv);
    if (!g_glc) fatal_box("OpenGL 3.3 indisponivel (driver de video?): %s", SDL_GetError());
    {   /* which GPU renders, and the automatic quality for it */
        typedef const unsigned char *(*gs_t)(unsigned);
        gs_t gs = (gs_t)SDL_GL_GetProcAddress("glGetString");
        const char *ven = gs ? (const char *)gs(0x1F00) : NULL, *ren = gs ? (const char *)gs(0x1F01) : NULL;
        int ded = is_dedicated_renderer(ren, ven), soft = ren && (strstr(ren, "llvmpipe") || strstr(ren, "softpipe"));
        if (scale <= 0) scale = ded ? 4 : soft ? 1 : 2;
        if (aniso < 0) aniso = ded ? 16 : 8;
        hlog("gpu: %s (%s) - %s; quality: internal scale %dx, anisotropy %dx", ren ? ren : "?", ven ? ven : "?",
             ded ? "dedicated GPU" : soft ? "software rendering" : "integrated GPU", scale, aniso);
        B->opt_scale = scale;
    }
    SDL_GL_MakeCurrent(g_win, g_glc);
    if (vsync) { if (SDL_GL_SetSwapInterval(-1)) SDL_GL_SetSwapInterval(1); } else SDL_GL_SetSwapInterval(0);
    rdr_gl_host_t host = { NULL, gl_getproc, gl_swap, win_size, glog_cb, NULL };
    rdr_gl_options_t opt = { scale, aniso, filt, 1 };
    R = rdr_gl_init(&host, &opt);
    if (!R) fatal_box("falha ao iniciar o OpenGL");
    { int dw, dh; SDL_GL_GetDrawableSize(g_win, &dw, &dh); B->win_w = dw; B->win_h = dh; hlog("window %dx%d (%s), vsync %d", dw, dh, g_fullscreen ? "fullscreen" : "window", vsync); }
    /* black first frame */
    { uint32_t black = 0; R->present_argb(&black, 1, 1, 1); }

    audio_open();
    if (getenv("GP3_VIRTUAL_JOY")) {   /* testing without hardware: a virtual 3-axis wheel */
        SDL_InitSubSystem(SDL_INIT_JOYSTICK);
        int (*attach)(int, int, int, int) = (int (*)(int, int, int, int))SDL_LoadFunction(SDL_LoadObject("libSDL2-2.0.so.0"), "SDL_JoystickAttachVirtual");
        int vi = attach ? attach(SDL_JOYSTICK_TYPE_WHEEL, 3, 12, 1) : -1;
        hlog("virtual test wheel attached (%d)", vi);
    }
    joy_init(B);

    /* extra game options */
    char *extra[16]; int ne = 0; static char spray[16];
    snprintf(spray, sizeof spray, "%d", ini_int("video", "spray_opacity", 60));
    extra[ne++] = "--spray"; extra[ne++] = spray;
    if (ini_int("game", "intro_videos", 1)) extra[ne++] = "--real-bink";
    if (!ini_int("game", "skip_asserts", 1)) setenv("GP3_STRICT_ASSERTS", "1", 1);
    if (!getenv("GP3_FORCE_GPX") && !ini_int("game", "gpxpatch", 1)) setenv("GP3_NO_GPX", "1", 1);
    if (!ini_int("game", "sound3d", 1)) setenv("GP3_NO_DS3D", "1", 1);
    /* replay ([game], same keys as gp3rt.ini on Windows) */
    if (!ini_int("game", "replay_keys", 1)) setenv("GP3_NO_REPLAYKEYS", "1", 1);
    if (!ini_int("game", "replay_legend", 1)) setenv("GP3_NO_REPLAYLEGEND", "1", 1);
    if (!ini_int("game", "replay_fx_fix", 1)) setenv("GP3_NO_FXFIX", "1", 1);
    if (!ini_int("game", "replay_2h", 1)) setenv("GP3_NO_REPLAY2H", "1", 1);
    if (!ini_int("game", "gp3replay", 1)) setenv("GP3_NO_GP3REPLAY", "1", 1);
    static char ws_w[16], ws_h[16], fov[16];
    int ww = ini_int("video", "wide_width", -1), wh = ini_int("video", "wide_height", -1);
    if (ww < 0 || wh < 0) { SDL_DisplayMode d; if (!SDL_GetDesktopDisplayMode(0, &d)) { ww = d.w; wh = d.h; } else ww = wh = 0; }
    if (ww > 0 && wh > 0) { snprintf(ws_w, sizeof ws_w, "%d", ww); snprintf(ws_h, sizeof ws_h, "%d", wh); extra[ne++] = "--wide"; extra[ne++] = ws_w; extra[ne++] = ws_h; }
    snprintf(fov, sizeof fov, "%d", ini_int("video", "hor_plus", 1) ? ini_int("video", "fov_percent", 100) : 0);
    extra[ne++] = "--fov"; extra[ne++] = fov;
    hlog("widescreen menu mode %dx%d, field of view %s%%", ww, wh, fov);
    if (script) script_load(script);
    spawn_game(fd, extra, ne);
    g_script_t0 = (uint64_t)SDL_GetTicks();
    SDL_StartTextInput();

    int running = 1; uint32_t quit_at = 0; uint64_t t0 = (uint64_t)SDL_GetTicks(), last_stat = t0, f0 = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: if (!quit_at) { push_event(BE_QUIT, 0, 0, 0, 0); quit_at = SDL_GetTicks(); hlog("quit requested"); } break;
            case SDL_KEYDOWN: case SDL_KEYUP: {
                int down = e.type == SDL_KEYDOWN;
                SDL_Scancode sc = e.key.keysym.scancode;
                if (down && sc == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
                    g_fullscreen = !g_fullscreen;
                    SDL_SetWindowFullscreen(g_win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    break;
                }
                if (down && sc == SDL_SCANCODE_F4 && (e.key.keysym.mod & KMOD_ALT)) { push_event(BE_QUIT, 0, 0, 0, 0); if (!quit_at) quit_at = SDL_GetTicks(); break; }
                if (sc == SDL_SCANCODE_PRINTSCREEN) { if (!down) { g_shot_pending = 1; g_shot_name[0] = 0; } break; }
                int scan, vk = sdl_to_vk(sc, &scan);
                /* keypad with NumLock off: Windows sends the navigation keys (Home, PgUp, Ins...),
                   which GP3 uses for the onboard cameras. SDL keeps reporting the keypad keys, so
                   the NumLock state decides. gp3rt.ini [game] numpad_keys: 0 = by NumLock (like
                   Windows), 1 = always navigation keys, 2 = always digits */
                if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_PERIOD) {
                    static int mode = -1; if (mode < 0) mode = ini_int("game", "numpad_keys", 0);
                    int numlock = (e.key.keysym.mod & KMOD_NUM) != 0;
                    if (mode == 1 || (mode == 0 && !numlock)) {
                        static const unsigned char nav[11] = { 0x23, 0x28, 0x22, 0x25, 0x0c, 0x27, 0x24, 0x26, 0x21, 0x2d, 0x2e };  /* KP 1..9, 0, . */
                        vk = nav[sc - SDL_SCANCODE_KP_1]; scan &= 0xff;
                    }
                }
                { static int nlog; if (nlog < 60 && down) { nlog++; hlog("key: sdl scancode %d (%s) sym %s numlock %d -> vk 0x%x scan 0x%x", (int)sc, SDL_GetScancodeName(sc), SDL_GetKeyName(e.key.keysym.sym), (e.key.keysym.mod & KMOD_NUM) != 0, vk, scan); } }
                if (vk) push_event(BE_KEY, vk, scan, down, 0);
                break; }
            case SDL_TEXTINPUT: {
                /* UTF-8 -> Latin-1 (the game uses the Windows-1252 code page) */
                const unsigned char *s = (const unsigned char *)e.text.text;
                while (*s) {
                    unsigned c = *s++;
                    if (c >= 0xc0 && c < 0xe0 && (*s & 0xc0) == 0x80) { c = ((c & 0x1f) << 6) | (*s++ & 0x3f); }
                    else if (c >= 0x80) { while ((*s & 0xc0) == 0x80) s++; continue; }
                    if (c >= 32 && c < 256) push_event(BE_CHAR, (int)c, 0, 0, 0);
                }
                break; }
            case SDL_MOUSEMOTION: { int x, y; map_mouse(e.motion.x, e.motion.y, &x, &y); push_event(BE_MOVE, x, y, 0, 0); break; }
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
                int x, y; map_mouse(e.button.x, e.button.y, &x, &y);
                int b = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
                if (b) push_event(BE_MOUSE, x, y, b, e.type == SDL_MOUSEBUTTONDOWN);
                break; }
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) { B->focused = 1; push_event(BE_ACTIVATE, 1, 0, 0, 0); sync_numlock(); }
                else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { B->focused = 0; push_event(BE_ACTIVATE, 0, 0, 0, 0); }
                else if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) { int dw, dh; SDL_GL_GetDrawableSize(g_win, &dw, &dh); B->win_w = dw; B->win_h = dh; }
                break;
            default: joy_event(&e); break;
            }
        }
        joy_poll();
        if (g_nsev && script_run() && !quit_at) { g_shot_pending = 1; snprintf(g_shot_name, sizeof g_shot_name, "final"); push_event(BE_QUIT, 0, 0, 0, 0); quit_at = SDL_GetTicks(); }
        if (max_seconds && !quit_at && (uint64_t)SDL_GetTicks() - g_script_t0 > (uint64_t)max_seconds * 1000) {
            hlog("max-seconds reached"); g_shot_pending = 1; snprintf(g_shot_name, sizeof g_shot_name, "final");
            push_event(BE_QUIT, 0, 0, 0, 0); quit_at = SDL_GetTicks();
        }
        if (!drain()) {
            /* nothing to do: sleep until the game writes (or 4 ms for input) */
            uint32_t w = A_LOAD(&B->cmd_wake);
            A_STORE(&B->host_waiting, 1);
            if (A_LOAD(&B->head) == B->tail) br_futex_wait(&B->cmd_wake, w, 4);
            A_STORE(&B->host_waiting, 0);
        }
        int st; pid_t r = waitpid(g_child, &st, WNOHANG);
        if (r == g_child) {
            if (WIFEXITED(st)) hlog("game exited (code %d)", WEXITSTATUS(st));
            else if (WIFSIGNALED(st)) {
                hlog("game process crashed (signal %d)", WTERMSIG(st));
                if (!quit_at) fatal_box("O jogo fechou inesperadamente (sinal %d). Veja gp3linux-game.log na pasta do jogo.", WTERMSIG(st));
            }
            running = 0;
        }
        if (quit_at && SDL_GetTicks() - quit_at > 6000) { hlog("game did not close; terminating"); B->quit = 2; kill(g_child, SIGTERM); running = 0; }
        uint64_t now = (uint64_t)SDL_GetTicks();
        if (now - last_stat >= 10000) {
            hlog("stats: %.1f fps, %llu draws/frame, audio underruns %u", (g_frames - f0) * 1000.0 / (now - last_stat),
                 (unsigned long long)(g_frames > f0 ? g_draws / (g_frames - f0) : 0), B->a_underruns);
            last_stat = now; f0 = g_frames; g_draws = 0;
        }
    }
    joy_shutdown();
    if (g_adev) SDL_CloseAudioDevice(g_adev);
    SDL_GL_DeleteContext(g_glc); SDL_DestroyWindow(g_win); SDL_Quit();
    hlog("bye");
    return 0;
}
