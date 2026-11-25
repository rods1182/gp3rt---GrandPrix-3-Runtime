/* Testing only (headless runner): trace GP3 replay ticks (1.13 and 2000).
   GP3_RPTRACE=<file>: at every replay-input record (live, tag 1) and read (replay, tag 0),
   writes {tag, ring index, sequence, race time} + the game-state block that GP3 snapshots.
   GP3_RPTRACE_FULL=t1,t2,...: also dumps the whole game image (<file>.W.<t> / .R.<t>) at the
   first tick of each mode whose race time >= t. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
int plat_patch_code(void *addr, const void *bytes, int n);
static FILE *g_f; static uint32_t g_seq; static long g_max = 1500L << 20, g_n;
static uint32_t g_blk, g_len, g_idx, g_time, g_ptr;
static const char *g_path; static uint32_t g_full[16]; static int g_nfull; static uint32_t g_done[2], g_pass[2][16]; static int g_autonull;
__attribute__((used)) void rp_log(int tag) {
    if (!g_f) return;
    { static int once[2]; if (!once[tag] && *(volatile int32_t *)(uintptr_t)g_time > 5000) { once[tag] = 1; uint32_t *sp = (uint32_t *)__builtin_frame_address(0); char b[400]; int n = 0, c = 0;
        for (int i = 0; i < 512 && c < 12; i++) if (sp[i] > 0x401000 && sp[i] < 0x52b000) { n += snprintf(b + n, sizeof b - n, " %08x", sp[i]); c++; }
        LOG("rptrace: chain tag %d:%s", tag, b); } }
    uint32_t t = *(volatile uint32_t *)(uintptr_t)g_time;
    if (g_autonull) { extern volatile int g_null_draw; int near = 0;
        for (int k = 0; k < g_nfull; k++) if (t + 1500 >= g_full[k] && t < g_full[k] + 1500) near = 1;
        g_null_draw = !near; }
    for (int k = 0; k < g_nfull; k++) if ((t + 3000 < g_full[k] || t > g_full[k] + 3000) && (g_done[tag] & (1u << k))) g_done[tag] &= ~(1u << k);
    for (int k = 0; k < g_nfull; k++) if (t >= g_full[k] && t < g_full[k] + 200 && !(g_done[tag] & (1u << k))) {
        g_done[tag] |= 1u << k; uint32_t pass = g_pass[tag][k]++;
        char fn[600]; snprintf(fn, sizeof fn, "%s.%c.%u.%u", g_path, tag ? 'W' : 'R', g_full[k], pass);
        FILE *o = fopen(fn, "wb");
        if (o) { uint32_t h[2] = { g_exe->base, t }; fwrite(h, 4, 2, o); fwrite((void *)(uintptr_t)g_exe->base, 1, g_exe->size, o); fclose(o); }
        LOG("rptrace: full dump %s (time %u)", fn, t);
        { extern void plat_screenshot(const char *) __attribute__((weak)); extern int g_probe_active __attribute__((weak));
          char sn[64]; snprintf(sn, sizeof sn, "rp_%c_%u_%u", tag ? 'W' : 'R', g_full[k], pass);
          if (plat_screenshot) { if (&g_probe_active) g_probe_active = 1; plat_screenshot(sn); } }
    }
    { extern volatile uint32_t g_alpha_draws, g_alpha_verts, g_puff_draws; static FILE *cf; static uint32_t ld, lv, lp;
      if (!cf) { char fn[600]; snprintf(fn, sizeof fn, "%s.cnt", g_path); cf = fopen(fn, "w"); }
      extern volatile int g_null_draw;
      if (cf) { fprintf(cf, "%d %u %u %u %d %u\n", tag, t, g_alpha_draws - ld, g_alpha_verts - lv, g_null_draw, g_puff_draws - lp); ld = g_alpha_draws; lv = g_alpha_verts; lp = g_puff_draws; if ((g_seq & 63) == 0) fflush(cf); } }
    if (g_n > g_max) return;
    uint32_t h[4] = { (uint32_t)tag, *(volatile uint32_t *)(uintptr_t)g_idx, g_seq++, t };
    fwrite(h, 4, 4, g_f);
    fwrite((void *)(uintptr_t)g_blk, 1, g_len, g_f);
    g_n += 16 + g_len;
    if ((g_seq & 255) == 0) fflush(g_f);
}
__attribute__((used)) uint32_t rp_ptr;
__attribute__((naked, used)) static void rp_stub_R(void) {
    __asm__ volatile("pushal\n pushl $0\n call rp_log\n addl $4,%esp\n popal\n movl rp_ptr,%eax\n movl (%eax),%eax\n ret\n");
}
__attribute__((naked, used)) static void rp_stub_W(void) {
    __asm__ volatile("pushal\n pushl $1\n call rp_log\n addl $4,%esp\n popal\n movl rp_ptr,%eax\n movl (%eax),%eax\n ret\n");
}
static int hook(uint32_t at, void *stub) {
    uint8_t want[5] = { 0xa1 }; memcpy(want + 1, &g_ptr, 4);
    if (memcmp((void *)(uintptr_t)at, want, 5)) { LOG("rptrace: %08x differs", at); return -1; }
    uint8_t c[5]; c[0] = 0xe8; int32_t r = (int32_t)((uintptr_t)stub - (at + 5)); memcpy(c + 1, &r, 4);
    return plat_patch_code((void *)(uintptr_t)at, c, 5);
}
void rptrace_install(void) {
    const char *p = getenv("GP3_RPTRACE"); if (!p) return;
    uint32_t rd, wr;
    if (!memcmp((void *)0x403bce, "\xa1\x8c\x3f\xb9\x00", 5)) { rd = 0x403bce; wr = 0x403c41; g_ptr = 0xb93f8c; g_blk = 0xb93f84; g_len = 0xbcec; }
    else if (!memcmp((void *)0x48b87c, "\xa1\xe0\xe1\x57\x00", 5)) { rd = 0x48b87c; wr = 0x48b8f0; g_ptr = 0x57e1e0; g_blk = 0x57e1d8; g_len = 0xc258; }
    else { LOG("rptrace: unknown build"); return; }
    g_idx = g_blk + 0x14; g_time = g_blk + 0x40; rp_ptr = g_ptr;
    if (getenv("GP3_RPTRACE_MB")) g_max = atol(getenv("GP3_RPTRACE_MB")) << 20;
    const char *fl = getenv("GP3_RPTRACE_FULL");
    for (const char *s = fl; s && *s && g_nfull < 16; ) { g_full[g_nfull++] = strtoul(s, (char **)&s, 10); if (*s == ',') s++; else break; }
    g_autonull = getenv("GP3_RPTRACE_AUTONULL") != NULL;
    g_path = p; g_f = fopen(p, "wb"); if (!g_f) return;
    hook(rd, (void *)rp_stub_R); hook(wr, (void *)rp_stub_W);
    LOG("rptrace: on -> %s (block %08x+%x)", p, g_blk, g_len);
}
