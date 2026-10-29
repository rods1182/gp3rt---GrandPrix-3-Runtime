/* GPU selection on machines with two graphics adapters (Intel/AMD integrated +
 * NVIDIA/AMD dedicated: laptops with Optimus / switchable graphics, or desktops
 * with the monitor on the motherboard output).
 *  - Windows' own per-application preference (what Settings > System > Display
 *    > Graphics writes): HKCU\Software\Microsoft\DirectX\UserGpuPreferences,
 *    value = full path of Gp3.exe, data "GpuPreference=2;" (high performance).
 *    Written at DLL load, before the OpenGL driver is loaded.
 *  - NvOptimusEnablement / AmdPowerXpressRequestHighPerformance exports
 *    (honoured by drivers that look at the loaded modules).
 *  - After the context exists we log which GPU actually renders and pick the
 *    automatic quality level (internal_scale / anisotropy = auto). */
#include "win32.h"
#include "../ddraw_int.h"
#include "win_cfg.h"

__attribute__((dllexport)) DWORD NvOptimusEnablement = 1;
__attribute__((dllexport)) int AmdPowerXpressRequestHighPerformance = 1;

#define HKEY_CURRENT_USER 0x80000001u
typedef uint32_t HKEY_;
DLLIMP LONG WINAPI RegCreateKeyExA(HKEY_, const char *, DWORD, char *, DWORD, DWORD, void *, HKEY_ *, DWORD *);
DLLIMP LONG WINAPI RegQueryValueExA(HKEY_, const char *, DWORD *, DWORD *, uint8_t *, DWORD *);
DLLIMP LONG WINAPI RegSetValueExA(HKEY_, const char *, DWORD, DWORD, const uint8_t *, DWORD);
DLLIMP LONG WINAPI RegDeleteValueA(HKEY_, const char *);
DLLIMP LONG WINAPI RegCloseKey(HKEY_);
typedef struct { DWORD cb; char name[32]; char string[128]; DWORD flags; char id[128]; char key[128]; } DISPLAY_DEVICEA_;
DLLIMP BOOL WINAPI EnumDisplayDevicesA(const char *, DWORD, DISPLAY_DEVICEA_ *, DWORD);

int g_gpu_mode;                 /* 0 auto (prefer dedicated), 1 dedicated, 2 integrated, 3 leave alone */
static char g_adapters[512];
static int g_has_dedicated, g_has_integrated;

static int is_integrated_name(const char *s) {
    return strstr(s, "Intel") || strstr(s, "UHD") || strstr(s, "Iris") || strstr(s, "Radeon(TM) Graphics") || strstr(s, "Radeon Graphics") || strstr(s, "Vega 8") || strstr(s, "Vega 6");
}
static int is_dedicated_name(const char *s) {
    if (is_integrated_name(s)) return 0;
    return strstr(s, "NVIDIA") || strstr(s, "GeForce") || strstr(s, "Quadro") || strstr(s, "RTX") || strstr(s, "GTX") || strstr(s, "Radeon") || strstr(s, "AMD") || strstr(s, "Arc");
}

void gpu_prefer(const char *ini) {
    char b[32];
    GetPrivateProfileStringA("video", "gpu", "auto", b, sizeof b, ini);
    g_gpu_mode = !strcmp(b, "dedicated") || !strcmp(b, "high") ? 1 : !strcmp(b, "integrated") || !strcmp(b, "low") ? 2 : !strcmp(b, "system") || !strcmp(b, "off") ? 3 : 0;
    /* adapters present (each adapter is listed once per output; dedup by name) */
    DISPLAY_DEVICEA_ d; size_t o = 0;
    for (DWORD i = 0; i < 16; i++) {
        memset(&d, 0, sizeof d); d.cb = sizeof d;
        if (!EnumDisplayDevicesA(NULL, i, &d, 0)) break;
        if (strstr(g_adapters, d.string)) continue;
        if (is_dedicated_name(d.string)) g_has_dedicated = 1; else if (is_integrated_name(d.string)) g_has_integrated = 1;
        o += (size_t)gp3_snprintf(g_adapters + o, sizeof g_adapters - o, "%s%s", o ? " | " : "", d.string);
        if (o >= sizeof g_adapters - 1) break;
    }
    LOG("graphics adapters: %s", g_adapters[0] ? g_adapters : "?");
    if (g_gpu_mode == 3) return;
    char exe[1024]; GetModuleFileNameA(0, exe, sizeof exe);
    HKEY_ k; DWORD disp;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, NULL, 0, 0x20006 /*KEY_WRITE*/ | 0x1 /*QUERY*/, NULL, &k, &disp)) {
        LOG("gpu: cannot open the Windows graphics preference key");
        return;
    }
    char cur[128] = ""; DWORD t = 0, n = sizeof cur - 1;
    int have = !RegQueryValueExA(k, exe, NULL, &t, (uint8_t *)cur, &n);
    const char *want = g_gpu_mode == 2 ? "GpuPreference=1;" : "GpuPreference=2;";
    if (g_gpu_mode == 0 && have && strstr(cur, "GpuPreference=") && !strstr(cur, "GpuPreference=0")) {
        LOG("gpu: keeping the choice made in Windows settings (%s)", cur);   /* user decided explicitly */
    } else if (!have || strcmp(cur, want)) {
        RegSetValueExA(k, exe, 0, 1 /*REG_SZ*/, (const uint8_t *)want, (DWORD)strlen(want) + 1);
        LOG("gpu: Windows graphics preference for Gp3.exe set to %s", g_gpu_mode == 2 ? "power saving (integrated)" : "high performance (dedicated)");
    }
    RegCloseKey(k);
}

/* called with the GL context current, before the renderer is created */
void gpu_after_context(void *(*getproc)(const char *), int *scale, int *aniso) {
    typedef const char *(WINAPI *gs_t)(unsigned);
    gs_t gs = (gs_t)getproc("glGetString");
    if (!gs) { HMODULE m = GetModuleHandleA("opengl32.dll"); if (m) gs = (gs_t)GetProcAddress(m, "glGetString"); }
    const char *ven = gs ? gs(0x1F00) : "?", *ren = gs ? gs(0x1F01) : "?";
    if (!ven) ven = "?"; if (!ren) ren = "?";
    int dedicated = is_dedicated_name(ren) || (strstr(ven, "NVIDIA") != NULL);
    LOG("gpu: rendering on %s (%s) - %s", ren, ven, dedicated ? "dedicated GPU" : "integrated/other GPU");
    if (!dedicated && g_has_dedicated && g_gpu_mode != 2)
        WARN("gpu: a dedicated GPU exists but the game is on the integrated one. Restart the game once (the preference is applied at "
             "startup); if it persists, set Gp3.exe to 'High performance' in Windows Settings > System > Display > Graphics, "
             "or in the NVIDIA Control Panel (Manage 3D settings > Program settings)");
    if (*scale <= 0) *scale = dedicated ? 4 : 2;
    if (*aniso < 0) *aniso = dedicated ? 16 : 8;
    LOG("gpu: quality %s -> internal scale %dx, anisotropy %dx", dedicated ? "high (dedicated GPU)" : "standard", *scale, *aniso);
}
