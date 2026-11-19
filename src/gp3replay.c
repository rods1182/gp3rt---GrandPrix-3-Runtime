/* GP3Replay.dll - replay of the whole race for Grand Prix 3 1.13, done in memory.
 *
 * Does in memory what Rafa's Gp3Video writes into gp3v.exe: the replay buffer moves to a
 * 13.5 MB block, the replay code gets Rafa's routines, and the length tables are raised. The
 * block is asked from Windows wherever there is room (gp3v.exe needs it right after the game
 * image); every address of it inside the patched code is relocated to where it landed.
 * Gp3.exe on disk is never touched.
 *
 * Loaded by gp3rt's ddraw.dll (Windows) and by GP3 Reborn (Linux) when it is in the game folder.
 * Any other loader works too: everything happens in DllMain. Only kernel32 is imported. */
#include <stdint.h>
#include "gp3replay_tab.h"

typedef void *HANDLE; typedef unsigned long DWORD; typedef int BOOL;
#define WINAPI __stdcall
__declspec(dllimport) void *WINAPI VirtualAlloc(void *, unsigned long, DWORD, DWORD);
__declspec(dllimport) BOOL WINAPI VirtualProtect(void *, unsigned long, DWORD, DWORD *);
__declspec(dllimport) BOOL WINAPI DisableThreadLibraryCalls(HANDLE);

#define GV_VERSION "1.0"
static int g_state = 1;          /* 1 = not run yet */
static char g_msg[200];

static void cat(char **p, char *e, const char *s) { while (*s && *p < e - 1) *(*p)++ = *s++; **p = 0; }
static void cathex(char **p, char *e, uint32_t v) {
    char b[11] = "0x"; for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15]; b[10] = 0; cat(p, e, b);
}
static void say(const char *a, uint32_t v, const char *b) {
    char *p = g_msg, *e = g_msg + sizeof g_msg; *p = 0;
    cat(&p, e, a); if (b) { cathex(&p, e, v); cat(&p, e, b); }
}
static uint32_t rd(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static int wr(uint32_t a, uint32_t v) {
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)a, 4, 0x40 /* PAGE_EXECUTE_READWRITE */, &old)) return -1;
    *(volatile uint32_t *)(uintptr_t)a = v;
    VirtualProtect((void *)(uintptr_t)a, 4, old, &old);
    return 0;
}

static int install(void) {
    unsigned i;
    for (i = 0; i < sizeof gv_check / sizeof gv_check[0]; i++)
        if (rd(gv_check[i][0]) != gv_check[i][1]) { say("off: this Gp3.exe is not GP3 1.13 without SafeDisc (GP3 2000 is not supported)", 0, 0); return -1; }
    unsigned same = 0, done = 0;
    for (i = 0; i < sizeof gv_patch / sizeof gv_patch[0]; i++) {
        uint32_t v = rd(gv_patch[i][0]);
        if (v == gv_patch[i][1]) same++;
        else if (v == gv_patch[i][2]) done++;
    }
    if (same != sizeof gv_patch / sizeof gv_patch[0]) {
        if (done > same) { say("off: this exe already has the whole-race replay patched in; nothing to do", 0, 0); return -2; }
        for (i = 0; i < sizeof gv_patch / sizeof gv_patch[0]; i++)
            if (rd(gv_patch[i][0]) != gv_patch[i][1]) break;
        say("off: Gp3.exe code differs from GP3 1.13 at ", gv_patch[i][0], " (another patch?)"); return -3;
    }
    uint8_t *code = VirtualAlloc(0, 0x1000, 0x3000 /* MEM_COMMIT|MEM_RESERVE */, 0x40 /* PAGE_EXECUTE_READWRITE */);
    uint8_t *data = VirtualAlloc(0, GV_DATA_SIZE, 0x3000, 0x04 /* PAGE_READWRITE */);
    if (!code || !data) { say("off: not enough memory for the replay buffer (13.5 MB)", 0, 0); return -4; }
    uint32_t c = (uint32_t)(uintptr_t)code, d = (uint32_t)(uintptr_t)data;
    for (i = 0; i < sizeof gv_page; i++) code[i] = gv_page[i];
    for (i = 0; i < sizeof gv_blob_out / sizeof gv_blob_out[0]; i++) {          /* calls back into the game */
        uint32_t s = c + gv_blob_out[i], t = GV_CODE + gv_blob_out[i] + 4 + *(uint32_t *)(uintptr_t)s;
        *(uint32_t *)(uintptr_t)s = t - (s + 4);
    }
    for (i = 0; i < sizeof gv_blob_data / sizeof gv_blob_data[0]; i++)
        *(uint32_t *)(uintptr_t)(c + gv_blob_data[i]) += d - GV_DATA;
    for (i = 0; i < sizeof gv_patch / sizeof gv_patch[0]; i++)
        if (wr(gv_patch[i][0], gv_patch[i][2])) { say("FAILED: could not write the game code at ", gv_patch[i][0], ""); return -5; }
    for (i = 0; i < sizeof gv_data_abs / sizeof gv_data_abs[0]; i++)
        wr(gv_data_abs[i], rd(gv_data_abs[i]) + (d - GV_DATA));
    for (i = 0; i < sizeof gv_code_rel / sizeof gv_code_rel[0]; i++) {           /* game -> Rafa's routines */
        uint32_t s = gv_code_rel[i], t = s + 4 + rd(s);
        wr(s, c + (t - GV_CODE) - (s + 4));
    }
    say("on - replay of the whole race; buffer at ", d, "");
    return 0;
}

/* for loaders that want to report: returns 0 when active, < 0 when not, and a one-line reason */
__declspec(dllexport) int __stdcall GP3Replay_Status(char *buf, int n) {
    if (g_state == 1) g_state = install();
    if (buf && n > 0) { int k = 0; const char *v = "GP3Replay.dll " GV_VERSION ": ";
        while (*v && k < n - 1) buf[k++] = *v++;
        for (const char *s = g_msg; *s && k < n - 1; s++) buf[k++] = *s;
        buf[k] = 0; }
    return g_state;
}
BOOL WINAPI DllMain(HANDLE h, DWORD reason, void *res) {
    (void)res;
    if (reason == 1) { DisableThreadLibraryCalls(h); if (g_state == 1) g_state = install(); }
    return 1;
}
