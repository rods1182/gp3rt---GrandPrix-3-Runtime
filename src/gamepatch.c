/* GP3 (1.13 and 2000) executable patches shared by the Windows ddraw.dll and the Linux build. */
#include <stdint.h>
#include <string.h>
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
