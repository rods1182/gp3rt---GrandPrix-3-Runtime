#include <stdio.h>
#include <stdint.h>
/* Headless platform: frames are written as PNG files, input comes from a
 * timed script. Used for automated testing of the runtime. */
#include "rt.h"
#include "plat.h"
#include <sys/stat.h>
#include <pthread.h>
#include <ctype.h>
#include <unistd.h>

static int g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 32;
static int g_frames;
static char g_pending_shot[128];
static pthread_mutex_t g_pmx = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* input script                                                          */
typedef struct { uint32_t t; int kind; int vk; int down; char arg[96]; } sevt_t;
enum { SE_KEY, SE_SHOT, SE_QUIT, SE_CHAR, SE_MOUSE, SE_ACTIVATE };
static sevt_t g_ev[1024];
static int g_nev, g_next;

static int vk_from_name(const char *s) {
    static const struct { const char *n; int vk; } t[] = {
        {"enter", 0x0d}, {"return", 0x0d}, {"esc", 0x1b}, {"escape", 0x1b}, {"space", 0x20}, {"left", 0x25}, {"up", 0x26},
        {"right", 0x27}, {"down", 0x28}, {"tab", 0x09}, {"shift", 0x10}, {"ctrl", 0x11}, {"alt", 0x12}, {"backspace", 0x08},
        {"pgup", 0x21}, {"pgdn", 0x22}, {"home", 0x24}, {"end", 0x23}, {"f1", 0x70}, {"f2", 0x71}, {"f3", 0x72}, {"f4", 0x73},
        {"f5", 0x74}, {"f6", 0x75}, {"f7", 0x76}, {"f8", 0x77}, {"f9", 0x78}, {"f10", 0x79}, {"f11", 0x7a}, {"f12", 0x7b}, {"prtsc", 0x2c}, {NULL, 0} };
    for (int i = 0; t[i].n; i++) if (!strcasecmp(t[i].n, s)) return t[i].vk;
    if (strlen(s) == 1) return toupper((unsigned char)s[0]);
    if (s[0] == '0' && s[1] == 'x') return (int)strtol(s, NULL, 16);
    return 0;
}
extern int vk_to_scancode(int vk);

void script_start(void) {
    if (!g_cfg.script[0]) return;
    FILE *f = fopen(g_cfg.script, "r");
    if (!f) { WARN("script %s not found", g_cfg.script); return; }
    char line[256]; uint32_t last = 0;
    while (fgets(line, sizeof line, f) && g_nev < 1020) {
        char *s = line; while (isspace((unsigned char)*s)) s++;
        if (!*s || *s == '#') continue;
        char tstr[32], cmd[32], a1[96] = "", a2[32] = "";
        if (sscanf(s, "%31s %31s %95s %31s", tstr, cmd, a1, a2) < 2) continue;
        uint32_t t = tstr[0] == '+' ? last + (uint32_t)atoi(tstr + 1) : (uint32_t)atoi(tstr);
        last = t;
        sevt_t *e = &g_ev[g_nev];
        memset(e, 0, sizeof *e); e->t = t;
        if (!strcmp(cmd, "key") || !strcmp(cmd, "tap")) {
            e->kind = SE_KEY; e->vk = vk_from_name(a1);
            if (!strcmp(a2, "down")) { e->down = 1; g_nev++; }
            else if (!strcmp(a2, "up")) { e->down = 0; g_nev++; }
            else { e->down = 1; g_nev++; g_ev[g_nev] = *e; g_ev[g_nev].down = 0; g_ev[g_nev].t = t + 80; g_nev++; }
        } else if (!strcmp(cmd, "shot")) { e->kind = SE_SHOT; snprintf(e->arg, sizeof e->arg, "%s", a1); g_nev++; }
        else if (!strcmp(cmd, "quit")) { e->kind = SE_QUIT; g_nev++; }
        else if (!strcmp(cmd, "activate")) { e->kind = SE_ACTIVATE; g_nev++; }
        else if (!strcmp(cmd, "type")) { e->kind = SE_CHAR; snprintf(e->arg, sizeof e->arg, "%s", a1); g_nev++; }
        else if (!strcmp(cmd, "click")) { e->kind = SE_MOUSE; e->vk = atoi(a1); e->down = atoi(a2); g_nev++; }
    }
    fclose(f);
    /* stable sort by time (insertion) */
    for (int i = 1; i < g_nev; i++) { sevt_t k = g_ev[i]; int j = i - 1; while (j >= 0 && g_ev[j].t > k.t) { g_ev[j + 1] = g_ev[j]; j--; } g_ev[j + 1] = k; }
    LOG("script: %d events", g_nev);
}

extern void win_post_message(HWND h, UINT msg, WPARAM w, LPARAM l);

/* analysis: GP3_CTL=<file> - live control; lines appended to the file run at once:
   key <name> | click <x> <y> | type <text> | shot <name> | dump <file> | quit */
extern volatile int g_null_draw;
static void ctl_poll(void) {
    static const char *fn; static long off; static uint32_t lastchk; static int init;
    if (!init) { init = 1; fn = getenv("GP3_CTL"); }
    if (!fn) return;
    uint32_t now = rt_ticks_ms(); if (now - lastchk < 100) return; lastchk = now;
    FILE *f = fopen(fn, "r"); if (!f) return;
    fseek(f, off, SEEK_SET); char line[256];
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n')) break;
        off = ftell(f);
        char cmd[32] = "", a1[200] = "", a2[32] = "";
        sscanf(line, "%31s %199s %31s", cmd, a1, a2);
        LOG("ctl: %s %s %s", cmd, a1, a2);
        if (!strcmp(cmd, "key")) { int vk = vk_from_name(a1); win_input_key(vk, vk_to_scancode(vk), 1); win_input_key(vk, vk_to_scancode(vk), 0); }
        else if (!strcmp(cmd, "keydown")) { int vk = vk_from_name(a1); win_input_key(vk, vk_to_scancode(vk), 1); }
        else if (!strcmp(cmd, "keyup")) { int vk = vk_from_name(a1); win_input_key(vk, vk_to_scancode(vk), 0); }
        else if (!strcmp(cmd, "click")) { win_input_mouse(atoi(a1), atoi(a2), 1, 1); }
        else if (!strcmp(cmd, "up")) { win_input_mouse(atoi(a1), atoi(a2), 1, 0); }
        else if (!strcmp(cmd, "move")) { win_input_mouse(atoi(a1), atoi(a2), 0, 0); }
        else if (!strcmp(cmd, "type")) { for (char *c = a1; *c; c++) win_input_char(*c == '_' ? ' ' : *c); }
        else if (!strcmp(cmd, "shot")) { extern int g_probe_active; g_probe_active = 1; plat_screenshot(a1); }
        else if (!strcmp(cmd, "dump")) { FILE *o = fopen(a1, "wb"); if (o) { fwrite((void *)(uintptr_t)0x401000, 1, 0xd72000 - 0x401000, o); fclose(o); LOG("ctl: dumped %s", a1); } }
        else if (!strcmp(cmd, "dumpr")) {   /* dumpr <file> <va>:<len> (hex) */
            unsigned va = 0, ln = 0; FILE *o;
            if (sscanf(a2, "%x:%x", &va, &ln) == 2 && (o = fopen(a1, "wb"))) { fwrite((void *)(uintptr_t)va, 1, ln, o); fclose(o); LOG("ctl: dumped %s", a1); }
        }
        else if (!strcmp(cmd, "null")) g_null_draw = atoi(a1);
        else if (!strcmp(cmd, "pokelist")) {   /* pokelist <file>: binary pairs [u32 va][u32 value] */
            FILE *d = fopen(a1, "rb"); uint32_t pr[2]; int k = 0;
            if (d) { while (fread(pr, 4, 2, d) == 2) { *(volatile uint32_t *)(uintptr_t)pr[0] = pr[1]; k++; } fclose(d); }
            LOG("ctl: poked list %s (%d)", a1, k);
        }
        else if (!strcmp(cmd, "poked")) { unsigned long va = strtoul(a1, NULL, 16); *(volatile int32_t *)(uintptr_t)va = (int32_t)strtol(a2, NULL, 0); LOG("ctl: poked %lx = %s", va, a2); }
        else if (!strcmp(cmd, "peekd")) { unsigned long va = strtoul(a1, NULL, 16); LOG("ctl: peek %lx = %d", va, *(volatile int32_t *)(uintptr_t)va); }
        else if (!strcmp(cmd, "pokeb")) { unsigned long va = strtoul(a1, NULL, 16); *(volatile uint8_t *)(uintptr_t)va = (uint8_t)strtoul(a2, NULL, 16); LOG("ctl: pokeb %lx = %s", va, a2); }
        else if (!strcmp(cmd, "poke")) {   /* poke <dumpfile> <va> <len>: copy bytes from a dump (based at 0x401000) */
            char df[200]; unsigned long va = 0, ln = 0;
            if (sscanf(line, "%*s %199s %li %li", df, (long *)&va, (long *)&ln) == 3) {
                FILE *d = fopen(df, "rb");
                if (d) { fseek(d, (long)(va - 0x401000), SEEK_SET); size_t r = fread((void *)(uintptr_t)va, 1, ln, d); fclose(d); LOG("ctl: poked %lx+%lx (%zu)", va, ln, r); }
            }
        }
        else if (!strcmp(cmd, "quit")) { if (g_log) fflush(g_log); _exit(0); }
    }
    fclose(f);
}
void plat_pump(void) {
    ctl_poll();
    uint32_t now = rt_ticks_ms() - 1000;   /* rt clock starts at 1s */
    if (g_cfg.max_seconds && now > (uint32_t)g_cfg.max_seconds * 1000) {
        LOG("max-seconds reached, exiting"); { extern void gpx_dump_modules(void); gpx_dump_modules(); }
        plat_screenshot("final");
        if (g_log) fflush(g_log);
        _exit(0);
    }
    while (g_next < g_nev && g_ev[g_next].t <= now) {
        sevt_t *e = &g_ev[g_next++];
        switch (e->kind) {
        case SE_KEY: LOG("script: key %02x %s", e->vk, e->down ? "down" : "up"); win_input_key(e->vk, vk_to_scancode(e->vk), e->down); break;
        case SE_SHOT: { extern int g_probe_active; g_probe_active = 1; plat_screenshot(e->arg);
            const char *md = getenv("GP3_MEMDUMP");
            if (md) { char fn[512]; snprintf(fn, sizeof fn, "%s_%s.bin", md, e->arg); FILE *f = fopen(fn, "wb");
                      if (f) { fwrite((void *)(uintptr_t)0x401000, 1, 0xd72000 - 0x401000, f); fclose(f); } }
            break; }
        case SE_ACTIVATE: { extern HWND g_main_hwnd; LOG("script: WM_ACTIVATE"); win_post_message(g_main_hwnd, 6, 1, 0); break; }
        case SE_QUIT: LOG("script: quit"); plat_screenshot("final"); if (g_log) fflush(g_log); _exit(0);
        case SE_CHAR: for (char *c = e->arg; *c; c++) win_input_char(*c == '_' ? ' ' : *c); break;
        case SE_MOUSE: win_input_mouse(e->vk >> 16, e->vk & 0xffff, 1, e->down); break;
        }
    }
}

/* ------------------------------------------------------------------ */
static uint32_t *g_last; static int g_lw, g_lh;

void plat_screenshot(const char *name) {
    pthread_mutex_lock(&g_pmx);
    if (g_last) {
        mkdir(g_cfg.dump_dir, 0755);
        char p[1024]; snprintf(p, sizeof p, "%s/%s.png", g_cfg.dump_dir, name);
        write_png(p, g_last, g_lw, g_lh, g_lw);
        LOG("screenshot %s (%dx%d)", p, g_lw, g_lh);
    } else snprintf(g_pending_shot, sizeof g_pending_shot, "%s", name);
    pthread_mutex_unlock(&g_pmx);
}

void plat_present(const uint32_t *xrgb, int w, int h, int pitch_px) {
    pthread_mutex_lock(&g_pmx);
    if (!g_last || g_lw != w || g_lh != h) { free(g_last); g_last = malloc((size_t)w * h * 4); g_lw = w; g_lh = h; }
    for (int y = 0; y < h; y++) memcpy(g_last + (size_t)y * w, xrgb + (size_t)y * pitch_px, (size_t)w * 4);
    g_frames++;
    { extern int g_probe_active; if (g_probe_active == 1) g_probe_active = 2; else if (g_probe_active == 2) g_probe_active = 0; }
    int dump = g_cfg.dump_every > 0 && g_frames % g_cfg.dump_every == 0;
    char pend[128]; snprintf(pend, sizeof pend, "%s", g_pending_shot); g_pending_shot[0] = 0;
    pthread_mutex_unlock(&g_pmx);
    if (dump || pend[0]) {
        mkdir(g_cfg.dump_dir, 0755);
        char p[1024];
        if (pend[0]) snprintf(p, sizeof p, "%s/%s.png", g_cfg.dump_dir, pend);
        else snprintf(p, sizeof p, "%s/frame_%05d.png", g_cfg.dump_dir, g_frames);
        write_png(p, xrgb, w, h, pitch_px);
        TRACE("dumped %s", p);
    }
}
void plat_client_size(int *w, int *h) { *w = g_mode_w; *h = g_mode_h; }
void plat_set_mode(int w, int h, int bpp) { g_mode_w = w; g_mode_h = h; g_mode_bpp = bpp; LOG("display mode -> %dx%dx%d", w, h, bpp); }
void plat_show_cursor(int show) { (void)show; }
void plat_message_box(const char *cap, const char *text) { (void)cap; (void)text; }
void video_init(void) {}
void plat_set_window(void *hwnd, unsigned flags) { (void)hwnd; (void)flags; }
/* no audio device in headless runs: DirectSound advances with the clock */
int plat_audio_rate(void) { return 0; }
uint32_t plat_audio_space(void) { return 0; }
void plat_audio_write(const int16_t *lr, uint32_t frames) { (void)lr; (void)frames; }
