/* GP3 (1.13 and 2000) executable patches shared by the Windows ddraw.dll and the Linux build. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "gamepatch.h"
#ifdef GP3_WIN
#include "win/win32.h"
#include "ddraw_int.h"
#else
#include "rt.h"
#endif
/* Hor+ field of view (same idea as WidePrix3 by "hatcher the patcher", which scales GP3's zoom by
   (4/3)/aspect). GP3 derives its focal length from the screen WIDTH (0x568164 = W*16384/640), so a
   wider screen just crops top and bottom. The stub replaces that store at 0x4082fb with a scale based
   on min(W, H*4/3), so every view (cockpit, TV cameras, any zoom) keeps the 4:3 vertical view and
   the sides grow. 4:3 and 5:4 modes are unchanged. g_ws_div = 1920 * 100 / fov_percent. */
/* GP3 1.13 and GP3 2000: the same code at different addresses (found by matching the
   machine code around each 1.13 site, with address fields masked; tools/xmatch.py). */
static const gp3_addrs_t g_builds[] = {
    { "GP3 1.13", 0x4082fb, { 0x89, 0x0d, 0x64, 0x81, 0x56, 0x00 }, 0x568164, 0xb2415c, 0xc64a80, { 0x5148da, 0x515953 }, 0 },
    { "GP3 2000", 0x490944, { 0x89, 0x0d, 0x30, 0xb8, 0x8d, 0x00 }, 0x8db830, 0x7ea18a, 0x579c48, { 0x44779a, 0x4487e3 }, 0x52ac50 - 0x4d5a5c },
};
static int g_addrs_done; static const gp3_addrs_t *g_addrs_cur;
void gp3_addrs_reset(void) { g_addrs_done = 0; g_addrs_cur = NULL; }   /* Linux: asked before Gp3.exe was loaded */
const gp3_addrs_t *gp3_addrs(void) {
    #define done g_addrs_done
    #define cur g_addrs_cur
    if (done) return cur;
    done = 1;
    for (unsigned i = 0; i < sizeof g_builds / sizeof g_builds[0]; i++) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)g_builds[i].fov_site;
        /* both sites lie inside the .text section of either build (always mapped) */
        if (!memcmp(p, g_builds[i].fov_orig, 6)) { cur = &g_builds[i]; break; }
    }
    LOG("game build: %s", cur ? cur->name : "unknown (widescreen and optimisations off)");
    return cur;
    #undef done
    #undef cur
}
__attribute__((used)) uint32_t g_ws_div __asm__("gp3_ws_div") = 1920;
__attribute__((used)) uint32_t g_ws_hptr __asm__("gp3_ws_hptr") = 0;
__attribute__((used)) uint32_t g_ws_xptr __asm__("gp3_ws_xptr") = 0;
__attribute__((naked)) static void ws_fov_stub(void) {
    __asm__ volatile(
        "push %eax\n push %edx\n push %ebx\n"
        "mov gp3_ws_hptr, %eax\n mov (%eax), %eax\n"
        "shl $16, %eax\n xor %edx, %edx\n mov gp3_ws_div, %ebx\n div %ebx\n"
        "cmp %ecx, %eax\n jb 1f\n mov %ecx, %eax\n"
        "1: mov gp3_ws_xptr, %ebx\n mov %eax, (%ebx)\n"
        "pop %ebx\n pop %edx\n pop %eax\n ret\n");
}
void gp3_hor_plus(int fov_pct) {
    const gp3_addrs_t *A = gp3_addrs();
    if (!A) { LOG("widescreen: unknown Gp3.exe build, Hor+ not applied"); return; }
    uint8_t *site = (uint8_t *)(uintptr_t)A->fov_site;
    g_ws_hptr = A->height; g_ws_xptr = A->xscale;
    if (fov_pct < 50) fov_pct = 50; if (fov_pct > 200) fov_pct = 200;
    g_ws_div = (uint32_t)(1920 * 100 / fov_pct);
    uint8_t p[6]; p[0] = 0xe8; int32_t rel = (int32_t)((uintptr_t)ws_fov_stub - (uintptr_t)(site + 5));
    memcpy(p + 1, &rel, 4); p[5] = 0x90;
    plat_patch_code(site, p, 6);
    LOG("widescreen: Hor+ field of view enabled (fov %d%%)", fov_pct);
}

void gp3_widen_modes(int w, int h) {
    const gp3_addrs_t *A = gp3_addrs();
    if (!A) { LOG("widescreen: unknown Gp3.exe build, resolution table not changed"); return; }
    uint32_t *t = (uint32_t *)(uintptr_t)A->res_table;
    if (plat_make_writable(t, 30 * 36)) return;
    int n = 0;
    for (int i = 0; i < 30; i++) {
        uint32_t *e = t + i * 9;
        if (e[0] == 1280 && e[1] == 1024) { e[0] = (uint32_t)w; e[1] = (uint32_t)h; n++; }
    }
    LOG("widescreen: %d resolution entries 1280x1024 -> %dx%d", n, w, h);
    extern void ddraw_add_mode(int w, int h); ddraw_add_mode(w, h);
}

/* GP3 1.13 language file (ldata\*.str): the game indexes 0xa70 strings, but the .str files that
   ship with it (and the community translations) hold about 0x9e8. The scan runs past the end of
   the file's buffer. Normally the heap behind it holds zeros (empty strings) and nothing shows;
   under GPxPatch's "Debug mode" Windows uses its debug heap, which is filled with non-zero
   patterns, and the scan runs off the heap: the game crashes before the menu (gp3.exe+0x105cc5).
   The buffer is allocated 16 KB larger and zeroed, so the missing strings are always empty. */
__attribute__((used)) uint32_t g_st_alloc __asm__("gp3_st_alloc") = 0;
__attribute__((naked)) static void st_alloc_stub(void) {
    __asm__ volatile(
        "push %edx\n add $0x4000, %edx\n call *gp3_st_alloc\n pop %edx\n"
        "test %eax, %eax\n jz 1f\n"
        "push %edi\n push %ecx\n push %eax\n"
        "mov %eax, %edi\n lea 0x4000(%edx), %ecx\n xor %eax, %eax\n cld\n rep stosb\n"
        "pop %eax\n pop %ecx\n pop %edi\n"
        "1: ret\n");
}
void gp3_fix_string_table(void) {
    const gp3_addrs_t *A = gp3_addrs();
    if (!A || A->fov_site != 0x4082fb) return;                    /* GP3 1.13 only */
    static const uint8_t want[] = { 0x8b, 0xd0, 0xb8, 0x73, 0x00, 0x00, 0x00, 0xe8, 0xa4, 0x5d, 0xfe, 0xff };
    uint8_t *site = (uint8_t *)(uintptr_t)0x50554e;
    if (memcmp(site, want, sizeof want)) { LOG("language file guard: code differs, not applied"); return; }
    g_st_alloc = 0x4eb2fe;
    uint8_t p[5]; p[0] = 0xe8; int32_t rel = (int32_t)((uintptr_t)st_alloc_stub - (uintptr_t)(site + 7 + 5));
    memcpy(p + 1, &rel, 4);
    if (plat_patch_code(site + 7, p, 5) == 0) LOG("language file guard: on (safe with short .str files and GPxPatch debug mode)");
}

/* 3D edge limit (GP3 1.13 and GP3 2000). Every frame the renderer gives each visible polygon edge a
   slot in a table of 2999 entries, backed by a 100 KB work area. Heavy tracks (many trees and
   objects, e.g. the 1998 "Belini" tracks) plus a far TV camera over many cars run past both: the
   edges that do not fit are marked "off" but keep the slot number from an earlier frame. The car
   bodies vanish (only the wheels remain), stray grey polygons appear, and when the stale slot points
   at reused data the game crashes (GP3 2000: gp3.exe+0x69480).
   Fix: the table and the work area move to bigger arrays here (16000 edges, 2 MB), and an edge that
   still does not fit gets a valid slot (the last one used this frame), so it can never crash. */
#define EDGE_CAP 16000
#define EDGE_POOL (2u << 20)
static uint32_t g_edge_tab[EDGE_CAP + 1];
static uint32_t g_edge_pool[EDGE_POOL / 4 + 64];
static uint32_t g_dummy_vtx[16], g_dummy_edge[8];
__attribute__((used)) uint32_t g_edge_cntp __asm__("gp3_edge_cntp") = 0;
__attribute__((used)) uint32_t g_edge_over __asm__("gp3_edge_over") = 0;
__attribute__((naked)) static void edge_over_stub(void) {
    __asm__ volatile(
        "movw $0x8080, (%edi)\n"
        "mov gp3_edge_cntp, %eax\n mov (%eax), %edx\n"
        "dec %edx\n jns 1f\n mov $" "16000" ", %edx\n"
        "1: mov %dx, 2(%edi)\n"
        "incl gp3_edge_over\n"
        "testl $0x3fff, gp3_edge_over\n jz 2f\n cmpl $1, gp3_edge_over\n jne 3f\n"
        "2: call gp3_edge_note\n"
        "3: popa\n ret\n");
}
__attribute__((used)) void edge_note(void) __asm__("gp3_edge_note");
__attribute__((used)) void edge_note(void) { LOG("3D edge limit: still too many edges in a frame (%u so far; drawn safely, no crash)", g_edge_over); }
static int text_range(uint8_t **lo, uint8_t **hi) {
    uint8_t *b = (uint8_t *)(uintptr_t)0x400000;
    if (b[0] != 'M' || b[1] != 'Z') return -1;
    uint8_t *pe = b + *(uint32_t *)(b + 0x3c);
    uint16_t ns = *(uint16_t *)(pe + 6), oh = *(uint16_t *)(pe + 20);
    uint8_t *s = pe + 24 + oh;
    for (int i = 0; i < ns; i++, s += 40)
        if (!memcmp(s, ".text", 6)) { *lo = b + *(uint32_t *)(s + 12); *hi = *lo + *(uint32_t *)(s + 8); return 0; }
    return -1;
}
uint32_t gp3_edge_overflows(void) { return g_edge_over; }
void gp3_raise_edge_limit(void) {
    uint8_t *lo, *hi;
    if (!gp3_addrs() || text_range(&lo, &hi)) { LOG("3D edge limit: unknown Gp3.exe build, not changed"); return; }
    /* the edge allocator: same code in both builds, addresses masked */
    static const uint8_t pat[] = { 0x60,0x8b,0xf8,0xa1, 0,0,0,0, 0x3d, 0,0,0,0, 0x72,0x15, 0xc6,0x05, 0,0,0,0, 0xff, 0x3d, 0,0,0,0,
        0x72,0x07, 0x66,0xc7,0x07,0x80,0x80, 0x61, 0xc3, 0x89,0x08, 0x89,0x50,0x04, 0x66,0x89,0x58,0x08, 0x66,0xc7,0x40,0x0c,0x00,0x00,
        0x8b,0xf1, 0x8b,0xda, 0x8b,0x15, 0,0,0,0, 0x81,0xfa,0xb7,0x0b,0x00,0x00, 0x7d,0xd8, 0x89,0x04,0x95, 0,0,0,0, 0x66,0x89,0x57,0x02 };
    static const uint8_t wild[][2] = { {4,4},{9,4},{17,4},{23,4},{57,4},{72,4} };
    uint8_t *f = NULL;
    for (uint8_t *p = lo; p + sizeof pat <= hi && !f; p++) {
        if (p[0] != 0x60 || p[1] != 0x8b || p[2] != 0xf8) continue;
        int ok = 1;
        for (unsigned i = 0; i < sizeof pat && ok; i++) {
            int w = 0; for (unsigned k = 0; k < 6; k++) if (i >= wild[k][0] && i < (unsigned)(wild[k][0] + wild[k][1])) w = 1;
            if (!w && p[i] != pat[i]) ok = 0;
        }
        if (ok) f = p;
    }
    if (!f) { LOG("3D edge limit: allocator not found, not changed"); return; }
    uint32_t P = *(uint32_t *)(f + 4), S = *(uint32_t *)(f + 9), H = *(uint32_t *)(f + 23);
    uint32_t C = *(uint32_t *)(f + 57), T = *(uint32_t *)(f + 72);
    /* collect every site first: "[reg*4 + table]" and "mov dword [pool_ptr], base" */
    enum { MAXS = 256 };
    static uint8_t *tab_sites[MAXS], *pool_sites[16];
    int nt = 0, np = 0; uint32_t base = 0;
    for (uint8_t *p = lo + 2; p + 4 <= hi; p++) {
        uint32_t v = *(uint32_t *)p;
        if (v == T && (p[-2] & 0xc7) == 0x04 && (p[-1] & 0xc7) == 0x85) { if (nt < MAXS) tab_sites[nt] = p; nt++; }
        if (v == P && p[-2] == 0xc7 && p[-1] == 0x05 && p + 8 <= hi) {
            uint32_t b = *(uint32_t *)(p + 4);
            if (!base) base = b;
            if (b == base) { if (np < 16) pool_sites[np] = p + 4; np++; }
        }
    }
    if (nt != 110 || np != 5 || !base || H <= S || S <= base) {
        LOG("3D edge limit: unexpected code (%d table uses, %d resets), not changed", nt, np); return;
    }
    uint32_t nT = (uint32_t)(uintptr_t)g_edge_tab, nB = (uint32_t)(uintptr_t)g_edge_pool;
    uint32_t nH = nB + EDGE_POOL, nS = nH - (H - S);
    for (int i = 0; i < nt; i++) plat_patch_code(tab_sites[i], &nT, 4);
    for (int i = 0; i < np; i++) plat_patch_code(pool_sites[i], &nB, 4);
    plat_patch_code(f + 9, &nS, 4);
    plat_patch_code(f + 23, &nH, 4);
    uint32_t cap = EDGE_CAP;
#ifndef GP3_WIN
    { const char *e = getenv("GP3_EDGE_CAP"); if (e && atoi(e) > 0 && atoi(e) < EDGE_CAP) cap = (uint32_t)atoi(e); }   /* testing */
#endif
    plat_patch_code(f + 63, &cap, 4);
    /* a free slot after the last: points at a harmless dummy edge (used only if nothing fit yet) */
    g_dummy_edge[0] = g_dummy_edge[1] = (uint32_t)(uintptr_t)g_dummy_vtx;
    g_edge_tab[EDGE_CAP] = (uint32_t)(uintptr_t)g_dummy_edge;
    g_edge_cntp = C;
    uint8_t j[7]; j[0] = 0xe9; int32_t rel = (int32_t)((uintptr_t)edge_over_stub - (uintptr_t)(f + 29 + 5));
    memcpy(j + 1, &rel, 4); j[5] = 0x90; j[6] = 0x90;
    plat_patch_code(f + 29, j, 7);
    /* the pool pointer may already hold the old base (set while loading): start from the new one */
    *(uint32_t *)(uintptr_t)P = nB; *(uint32_t *)(uintptr_t)C = 0;
    LOG("3D edge limit: 2999 -> %u edges, work area %u KB -> %u KB (allocator at %p)", cap, (H - base) >> 10, EDGE_POOL >> 10, (void *)f);
}
