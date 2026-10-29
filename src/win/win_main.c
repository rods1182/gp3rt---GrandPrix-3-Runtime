/* gp3rt ddraw.dll for Windows: entry point, configuration, logging, exports
 * and the few import-table hooks GP3 needs (virtual game CD, DirectPlay). */
#include "win32.h"
#include "../ddraw_int.h"
#include "win_cfg.h"

/* ------------------------------------------------------------------ */
/* globals expected by the shared code                                  */
int g_log_level = 1;
extern int g_overlay_locks, g_spray_opacity, g_nofault;
FILE *g_log;
config_t g_cfg;
int g_trace_d3d;
char g_capture_path[512];
win_cfg_t g_wcfg;
char g_game_dir[1024];
static char g_ini_path[1100];
char g_ini_path_ext[1100];
static int64_t g_qpf, g_qpc0;

uint64_t rt_now_us(void) {
    int64_t c; QueryPerformanceCounter(&c);
    if (!g_qpf) { QueryPerformanceFrequency(&g_qpf); g_qpc0 = c; }
    return (uint64_t)((c - g_qpc0) * 1000000.0 / (double)g_qpf) + 1000000;
}
static os_mutex_t g_logmx; static int g_logmx_ok;
void rt_log(int lvl, const char *fmt, ...) {
    if (lvl > g_log_level || !g_log) return;
    char b[2048];
    va_list ap; va_start(ap, fmt); gp3_vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (g_logmx_ok) os_mutex_lock(&g_logmx);
    fprintf(g_log, "[%9.3f] %s\n", rt_now_us() / 1e6 - 1.0, b);
    fflush(g_log);
    if (g_logmx_ok) os_mutex_unlock(&g_logmx);
}
void rt_fatal(const char *fmt, ...) {
    char b[2048];
    va_list ap; va_start(ap, fmt); gp3_vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (g_log) { fprintf(g_log, "FATAL: %s\n", b); fflush(g_log); }
    MessageBoxA(0, b, "GP3 runtime", 0x10);
    ExitProcess_(2);
    __builtin_unreachable();
}
void os_sleep_us(uint64_t us) {
    /* waitable timer: much finer than Sleep() */
    static HANDLE t;
    if (!t) t = CreateWaitableTimerA(NULL, 1, NULL);
    if (t && us > 200) {
        int64_t due = -(int64_t)us * 10;
        if (SetWaitableTimer(t, &due, 0, NULL, NULL, 0)) { WaitForSingleObject(t, 0xffffffffu); return; }
    }
    Sleep((DWORD)((us + 999) / 1000));
}

/* exports registered by ddraw.c */
static const export_t *g_tabs[8]; static int g_ntabs;
void rt_register_exports(const export_t *tab) { if (g_ntabs < 8) g_tabs[g_ntabs++] = tab; }
void *rt_find_export(const char *dll, const char *name, int ordinal) {
    (void)ordinal;
    for (int t = 0; t < g_ntabs; t++) for (const export_t *e = g_tabs[t]; e->dll; e++)
        if (!strcmp(e->dll, dll) && e->name && !strcmp(e->name, name)) return e->fn;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* configuration (gp3rt.ini next to Gp3.exe)                            */
static void cfg_load(void) {
    char *ini = g_ini_path; gp3_snprintf(ini, sizeof g_ini_path, "%s\\gp3rt.ini", g_game_dir);
    char b[64];
    GetPrivateProfileStringA("video", "mode", "fullscreen", b, sizeof b, ini);
    g_wcfg.windowed = !strcmp(b, "window") || !strcmp(b, "windowed");
    g_wcfg.window_scale = (int)GetPrivateProfileIntA("video", "window_scale", 0, ini);
    GetPrivateProfileStringA("video", "renderer", "opengl", b, sizeof b, ini);
    g_wcfg.software = !strcmp(b, "software");
    g_wcfg.internal_scale = (int)GetPrivateProfileIntA("video", "internal_scale", 0, ini);   /* 0 / "auto": chosen by GPU */
    g_wcfg.filtering = (int)GetPrivateProfileIntA("video", "force_filtering", 1, ini);
    GetPrivateProfileStringA("video", "anisotropy", "auto", b, sizeof b, ini);
    g_wcfg.anisotropy = !strcmp(b, "auto") ? -1 : atoi(b);
    g_wcfg.vsync = (int)GetPrivateProfileIntA("video", "vsync", 1, ini);
    GetPrivateProfileStringA("video", "aspect", "keep", b, sizeof b, ini);
    g_wcfg.stretch = !strcmp(b, "stretch");
    g_wcfg.virtual_cd = (int)GetPrivateProfileIntA("game", "virtual_cd", 1, ini);
    g_wcfg.multiplayer = (int)GetPrivateProfileIntA("game", "directplay", 0, ini);
    g_wcfg.lazy_readback = (int)GetPrivateProfileIntA("advanced", "lazy_readback", 1, ini);
    { extern int g_overlay_locks; g_overlay_locks = (int)GetPrivateProfileIntA("advanced", "overlay_locks", 1, ini); }
    { extern int g_spray_opacity; int v = (int)GetPrivateProfileIntA("video", "spray_opacity", 100, ini);
      g_spray_opacity = v < 0 ? 0 : v > 100 ? 100 : v;
      extern int g_nofault; DLLIMP BOOL WINAPI IsDebuggerPresent(void);
      int ef = (int)GetPrivateProfileIntA("advanced", "exception_free", -1, ini);   /* -1 = automatic */
      g_nofault = ef < 0 ? (IsDebuggerPresent() != 0) : ef != 0;
      extern int g_spray_near_fade; g_spray_near_fade = (int)GetPrivateProfileIntA("video", "spray_near_fade", 1, ini); }
    g_log_level = (int)GetPrivateProfileIntA("debug", "log_level", 1, ini);
    g_trace_d3d = (int)GetPrivateProfileIntA("debug", "trace_d3d", 0, ini);
    g_wcfg.emulate_ctx = (int)GetPrivateProfileIntA("debug", "thread_bound", 0, ini);
    GetPrivateProfileStringA("debug", "capture", "", g_capture_path, sizeof g_capture_path, ini);
    { extern int g_frame_dump; extern char g_frame_dump_dir[];
      g_frame_dump = (int)GetPrivateProfileIntA("debug", "frame_dump", 1, ini);
      gp3_snprintf(g_frame_dump_dir, 600, "%s\\screens", g_game_dir); }
    if (g_wcfg.internal_scale < 0) g_wcfg.internal_scale = 0;
    if (g_wcfg.internal_scale > 4) g_wcfg.internal_scale = 4;
    gp3_snprintf(g_cfg.dump_dir, sizeof g_cfg.dump_dir, "%s\\screens", g_game_dir);
}

/* ------------------------------------------------------------------ */
/* import table patching                                                */
static void *iat_hook(HMODULE mod, const char *dll, const char *fn, void *repl) {
    uint8_t *base = (uint8_t *)mod;
    uint32_t pe = *(uint32_t *)(base + 0x3c);
    uint32_t rva = *(uint32_t *)(base + pe + 0x80);   /* import directory */
    if (!rva) return NULL;
    for (IMPORT_DESC *d = (IMPORT_DESC *)(base + rva); d->name; d++) {
        if (_stricmp((const char *)(base + d->name), dll)) continue;
        uint32_t *names = (uint32_t *)(base + (d->orig_thunk ? d->orig_thunk : d->first_thunk));
        void **iat = (void **)(base + d->first_thunk);
        for (int i = 0; names[i]; i++) {
            if (names[i] & 0x80000000u) continue;
            if (strcmp((const char *)(base + names[i] + 2), fn)) continue;
            DWORD old;
            void *orig = iat[i];
            VirtualProtect(&iat[i], sizeof(void *), PAGE_READWRITE, &old);
            iat[i] = repl;
            VirtualProtect(&iat[i], sizeof(void *), old, &old);
            return orig;
        }
    }
    return NULL;
}

/* ---- virtual game CD --------------------------------------------------
 * Gp3.exe looks for a CD-ROM drive and opens "<drive>:\grand prix 3\bitmaps\
 * gp3art5.bin". We add a virtual drive whose "\Grand Prix 3\" is the
 * installation directory (a full install contains every file) and hide real
 * optical drives from the game so that it always picks ours. */
static char g_vcd = 0;
static DWORD (WINAPI *o_GetLogicalDrives)(void);
static UINT (WINAPI *o_GetDriveTypeA)(const char *);
static BOOL (WINAPI *o_GetVolumeInformationA)(const char *, char *, DWORD, DWORD *, DWORD *, DWORD *, char *, DWORD);
static HANDLE (WINAPI *o_CreateFileA)(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
static DWORD (WINAPI *o_GetFileAttributesA)(const char *);
static HANDLE (WINAPI *o_FindFirstFileA)(const char *, void *);
static BOOL (WINAPI *o_SetCurrentDirectoryA)(const char *);

static int is_vcd(const char *p) { return p && p[0] && (p[0] | 32) == (g_vcd | 32) && p[1] == ':'; }
static const char *vcd_map(const char *p, char *buf, size_t n) {
    if (!is_vcd(p)) return p;
    const char *r = p + 2;
    while (*r == '\\' || *r == '/') r++;
    if (!_strnicmp(r, "grand prix 3", 12) && (r[12] == 0 || r[12] == '\\' || r[12] == '/')) { r += 12; while (*r == '\\' || *r == '/') r++; }
    gp3_snprintf(buf, n, "%s\\%s", g_game_dir, r);
    return buf;
}
static DWORD WINAPI h_GetLogicalDrives(void) { return o_GetLogicalDrives() | (1u << (g_vcd - 'A')); }
static UINT WINAPI h_GetDriveTypeA(const char *root) {
    if (is_vcd(root)) return 5;               /* DRIVE_CDROM */
    UINT t = o_GetDriveTypeA(root);
    return t == 5 ? 1 : t;                    /* hide real optical drives: DRIVE_NO_ROOT_DIR */
}
static BOOL WINAPI h_GetVolumeInformationA(const char *root, char *name, DWORD nl, DWORD *serial, DWORD *maxc, DWORD *flags, char *fs, DWORD fl) {
    if (!is_vcd(root)) return o_GetVolumeInformationA(root, name, nl, serial, maxc, flags, fs, fl);
    if (name && nl) gp3_snprintf(name, nl, "GP3");
    if (serial) *serial = 0x19980701;
    if (maxc) *maxc = 255;
    if (flags) *flags = 0;
    if (fs && fl) gp3_snprintf(fs, fl, "CDFS");
    return 1;
}
static HANDLE WINAPI h_CreateFileA(const char *p, DWORD a, DWORD s, void *sa, DWORD d, DWORD f, HANDLE t) {
    char b[1200]; const char *m = vcd_map(p, b, sizeof b);
    if (m != p) LOG("virtual CD: %s -> %s", p, m);
    return o_CreateFileA(m, a, s, sa, d, f, t);
}
static DWORD WINAPI h_GetFileAttributesA(const char *p) { char b[1200]; return o_GetFileAttributesA(vcd_map(p, b, sizeof b)); }
static HANDLE WINAPI h_FindFirstFileA(const char *p, void *fd) { char b[1200]; return o_FindFirstFileA(vcd_map(p, b, sizeof b), fd); }
static BOOL WINAPI h_SetCurrentDirectoryA(const char *p) { char b[1200]; return o_SetCurrentDirectoryA(vcd_map(p, b, sizeof b)); }

/* ---- DirectPlay ----------------------------------------------------------
 * GP3 instantiates DirectPlay at start-up. On Windows 10/11 DirectPlay is an
 * optional component and Windows interrupts the game asking to install it.
 * Unless multiplayer is enabled in gp3rt.ini we report it as unavailable. */
static LONG (WINAPI *o_CoCreateInstance)(const uint8_t *, void *, DWORD, const uint8_t *, void **);
static LONG WINAPI h_CoCreateInstance(const uint8_t *clsid, void *outer, DWORD ctx, const uint8_t *iid, void **out) {
    static const uint8_t dplay[16] = { 0x20, 0x6D, 0xEB, 0xD1, 0x23, 0x89, 0xD0, 0x11, 0x9D, 0x97, 0x00, 0xA0, 0xC9, 0x0A, 0x43, 0xCB };
    static const uint8_t lobby[16] = { 0x10, 0xF8, 0xE5, 0x2F, 0xA5, 0xB2, 0xD0, 0x11, 0xA7, 0x87, 0x00, 0x00, 0xF8, 0x03, 0xAB, 0xFC };
    if (!g_wcfg.multiplayer && clsid && (!memcmp(clsid, dplay, 16) || !memcmp(clsid, lobby, 16))) {
        if (out) *out = NULL;
        return (LONG)0x80040154;   /* REGDB_E_CLASSNOTREG */
    }
    return o_CoCreateInstance(clsid, outer, ctx, iid, out);
}

/* ---- message loop: deferred/coalesced presents are shown when the game pumps messages */
extern void ddraw_flush_present(void);
static BOOL (WINAPI *o_PeekMessageA)(void *, HWND, UINT, UINT, UINT);
static BOOL (WINAPI *o_GetMessageA)(void *, HWND, UINT, UINT);
extern void ddraw_flush_present_nb(void);
extern void win_pump_mark(int phase);   /* watchdog bookkeeping (win_plat.c) */
static BOOL WINAPI h_PeekMessageA(void *m, HWND h, UINT a, UINT b, UINT r) { win_pump_mark(0); ddraw_flush_present_nb(); BOOL x = o_PeekMessageA(m, h, a, b, r); win_pump_mark(0); return x; }
extern void win_ctx_release(void);
extern void win_ctx_release_if_wanted(void);
static BOOL WINAPI h_GetMessageA(void *m, HWND h, UINT a, UINT b) { ddraw_flush_present_nb(); win_ctx_release_if_wanted(); win_pump_mark(1); BOOL x = o_GetMessageA(m, h, a, b); win_pump_mark(2); win_ctx_release_if_wanted(); return x; }
/* GP3's game thread sends WM_USER requests (mode switches, loading screens) to its
 * window thread and waits: lend it the render context for the duration */
static LRESULT (WINAPI *o_SendMessageA)(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI h_SendMessageA(HWND h, UINT m, WPARAM w, LPARAM l) { win_ctx_release(); return o_SendMessageA(h, m, w, l); }
/* GP3 copies its frames to the primary surface at ClientToScreen(window) - DirectDraw's
 * convention for windowed programs, where the primary surface is the whole desktop. Ours
 * is the game's own picture, so in window mode every frame landed shifted by the window's
 * position and cut off. The game window's client area is reported at 0,0, as it is in
 * full screen. */
static BOOL (WINAPI *o_ClientToScreen)(HWND, WPOINT *);
/* GP3 sets its own window style (a borderless popup) and size on every mode switch: the
   window is ours to lay out (title bar in window mode, borderless in full screen) */
static LONG (WINAPI *o_SetWindowLongA)(HWND, int, LONG);
extern HWND win_game_hwnd(void);
static LONG WINAPI h_SetWindowLongA(HWND h, int i, LONG v) {
    if (h && h == win_game_hwnd() && (i == -16 || i == -20)) {
        static int n; if (n < 4) { n++; LOG("game asked for window %s %x: kept ours", i == -16 ? "style" : "ex-style", (unsigned)v); }
        return GetWindowLongA(h, i);
    }
    return o_SetWindowLongA(h, i, v);
}
/* GP3 hides the Windows cursor (it draws its own in the menus). The wish is remembered and
   applied only over the picture, so the cursor shows on the title bar and outside */
/* ...and GP3 sizes its screen from its window: in window mode it is told what it sees in
   full screen (the window covering the monitor at 0,0), otherwise starting a race or
   changing the resolution in a window crashed it */
static BOOL (WINAPI *o_GetWindowRect)(HWND, WRECT *);
static BOOL (WINAPI *o_GetClientRect)(HWND, WRECT *);
static int game_rect(HWND h, WRECT *r, const char *what) {
    if (!h || h != win_game_hwnd() || !r || !g_wcfg.windowed) return 0;
    MONITORINFO mi; mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(h, 2 /* MONITOR_DEFAULTTONEAREST */), &mi);
    r->left = 0; r->top = 0; r->right = mi.rcMonitor.right - mi.rcMonitor.left; r->bottom = mi.rcMonitor.bottom - mi.rcMonitor.top;
    static int n; if (n < 6) { n++; LOG("%s of the game window in window mode: %dx%d at 0,0 reported (as in full screen)", what, (int)r->right, (int)r->bottom); }
    return 1;
}
static BOOL WINAPI h_GetWindowRect(HWND h, WRECT *r) { return game_rect(h, r, "GetWindowRect") ? 1 : o_GetWindowRect(h, r); }
static BOOL WINAPI h_GetClientRect(HWND h, WRECT *r) { return game_rect(h, r, "GetClientRect") ? 1 : o_GetClientRect(h, r); }
static int (WINAPI *o_ShowCursor)(BOOL);
volatile int g_game_cursor = 0;
static int WINAPI h_ShowCursor(BOOL show) { g_game_cursor += show ? 1 : -1; return g_game_cursor; }
extern HWND win_game_hwnd(void);
static BOOL WINAPI h_ClientToScreen(HWND h, WPOINT *p) {
    if (h && h == win_game_hwnd() && p) {
        static int n;
        if (n < 3) { WPOINT q = *p; o_ClientToScreen(h, &q); if (q.x != p->x || q.y != p->y) { n++; LOG("ClientToScreen of the game window: %d,%d reported instead of %d,%d", (int)p->x, (int)p->y, (int)q.x, (int)q.y); } }
        return 1;
    }
    return o_ClientToScreen(h, p);
}

/* ---- clean exit ----
 * GP3 leaving with the OpenGL context alive can leave Gp3.exe hanging in the
 * background (window gone, process alive). That zombie keeps keyboard/mouse
 * (DirectInput) and the foreground, so the next start gets no input. */
DLLIMP HANDLE WINAPI GetCurrentProcess(void);
DLLIMP BOOL WINAPI TerminateProcess(HANDLE, UINT);
DLLIMP BOOL WINAPI CloseHandle(HANDLE);
DLLIMP HANDLE WINAPI OpenProcess(DWORD, BOOL, DWORD);
DLLIMP BOOL WINAPI QueryFullProcessImageNameA(HANDLE, DWORD, char *, DWORD *);
DLLIMP HANDLE WINAPI CreateToolhelp32Snapshot(DWORD, DWORD);
typedef struct { DWORD size, usage, pid; uintptr_t heap; DWORD module, threads, parent; LONG pri; DWORD flags; char exe[260]; } PROCENTRY_;
DLLIMP BOOL WINAPI Process32First(HANDLE, PROCENTRY_ *);
DLLIMP BOOL WINAPI Process32Next(HANDLE, PROCENTRY_ *);
DLLIMP DWORD WINAPI GetCurrentProcessId(void);
DLLIMP BOOL WINAPI IsDebuggerPresent(void);
static int dbg_present(void) { return IsDebuggerPresent() != 0; }
static void (WINAPI *o_ExitProcess)(UINT);
static void WINAPI h_ExitProcess(UINT code);
void win_clean_exit(UINT code) { h_ExitProcess(code); }
static int g_fast_exit = 1;
static void WINAPI h_ExitProcess(UINT code) {
    extern void win_gl_shutdown(void);
    LOG("game exiting (code %u): releasing OpenGL and closing", code);
    win_gl_shutdown();
    if (g_log) fflush(g_log);
    if (g_fast_exit) TerminateProcess(GetCurrentProcess(), code);   /* skips driver DLL teardown that can hang */
    o_ExitProcess(code);
}
/* launchers (GPxPatch "CPU" option) may pin the game to one CPU core. With a real GPU
   driver (its worker threads) that starves the window thread: no keyboard/mouse,
   "not responding". Give the process all cores back unless asked not to. */
DLLIMP BOOL WINAPI GetProcessAffinityMask(HANDLE, uintptr_t *, uintptr_t *);
DLLIMP BOOL WINAPI SetProcessAffinityMask(HANDLE, uintptr_t);
int win_fix_affinity(int quiet) {
    static int keep = -1;
    if (keep < 0) keep = (int)GetPrivateProfileIntA("advanced", "keep_cpu_affinity", 0, g_ini_path);
    uintptr_t pm = 0, sm = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) return 0;
    int pc = __builtin_popcount((unsigned)pm), sc = __builtin_popcount((unsigned)sm);
    if (pc < sc && pc <= 1) {
        if (keep) { static int told; if (!told++) LOG("cpu: game limited to %d of %d cores by the launcher (kept: keep_cpu_affinity=1)", pc, sc); return 0; }
        BOOL ok = SetProcessAffinityMask(GetCurrentProcess(), sm);
        LOG("cpu: game was limited to %d of %d cores (e.g. GPxPatch CPU option): using all cores -> %s", pc, sc, ok ? "ok" : "failed");
        return 1;
    }
    if (!quiet) LOG("cpu: %d of %d cores", pc, sc);
    return 0;
}
static void kill_stale_instances(void) {
    char me[1024]; GetModuleFileNameA(0, me, sizeof me);
    HANDLE snap = CreateToolhelp32Snapshot(0x2 /*TH32CS_SNAPPROCESS*/, 0);
    if (snap == (HANDLE)INVALID_HANDLE_VALUE || !snap) return;
    PROCENTRY_ pe; memset(&pe, 0, sizeof pe); pe.size = sizeof pe;
    DWORD self = GetCurrentProcessId();
    const char *base = strrchr(me, '\\'); base = base ? base + 1 : me;
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (pe.pid == self || _stricmp(pe.exe, base)) continue;
        HANDLE h = OpenProcess(0x1000 /*QUERY_LIMITED_INFORMATION*/ | 0x1 /*TERMINATE*/, 0, pe.pid);
        if (!h) { LOG("another %s is running (pid %u) and cannot be closed by us: close it in the Task Manager if the game gets no input", pe.exe, pe.pid); continue; }
        char path[1024]; DWORD n = sizeof path;
        if (QueryFullProcessImageNameA(h, 0, path, &n) && !_stricmp(path, me)) {
            BOOL t = TerminateProcess(h, 1);
            LOG("closed a leftover %s from a previous run (pid %u): %s", pe.exe, pe.pid, t ? "ok" : "failed");
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
}

/* experimental: widen the game's 1280x1024 entries of its resolution table (0xc64a80, 36-byte entries) */
#include "../gamepatch.h"
static void install_hooks(void) {
    HMODULE exe = GetModuleHandleA(0);
    if (g_wcfg.virtual_cd) {
        DWORD used = GetLogicalDrives();
        for (char c = 'P'; c <= 'Z' && !g_vcd; c++) if (!(used & (1u << (c - 'A')))) g_vcd = c;
        for (char c = 'O'; c >= 'D' && !g_vcd; c--) if (!(used & (1u << (c - 'A')))) g_vcd = c;
        if (g_vcd) {
            o_GetLogicalDrives = iat_hook(exe, "kernel32.dll", "GetLogicalDrives", (void *)h_GetLogicalDrives);
            o_GetDriveTypeA = iat_hook(exe, "kernel32.dll", "GetDriveTypeA", (void *)h_GetDriveTypeA);
            o_GetVolumeInformationA = iat_hook(exe, "kernel32.dll", "GetVolumeInformationA", (void *)h_GetVolumeInformationA);
            o_CreateFileA = iat_hook(exe, "kernel32.dll", "CreateFileA", (void *)h_CreateFileA);
            o_GetFileAttributesA = iat_hook(exe, "kernel32.dll", "GetFileAttributesA", (void *)h_GetFileAttributesA);
            o_FindFirstFileA = iat_hook(exe, "kernel32.dll", "FindFirstFileA", (void *)h_FindFirstFileA);
            o_SetCurrentDirectoryA = iat_hook(exe, "kernel32.dll", "SetCurrentDirectoryA", (void *)h_SetCurrentDirectoryA);
            LOG("virtual CD drive %c: -> %s (hooks: %s)", g_vcd, g_game_dir, o_GetDriveTypeA && o_CreateFileA ? "ok" : "MISSING");
            /* the KERNEL32 import name may be spelled differently by the no-CD build ("KeRNeL32.dll"): iat_hook compares case-insensitively */
        }
    }
    o_CoCreateInstance = iat_hook(exe, "ole32.dll", "CoCreateInstance", (void *)h_CoCreateInstance);
    { int ww = (int)GetPrivateProfileIntA("video", "wide_width", 0, g_ini_path), wh = (int)GetPrivateProfileIntA("video", "wide_height", 0, g_ini_path);
      if (ww < 0 || wh < 0) { ww = GetSystemMetrics(0 /*SM_CXSCREEN*/); wh = GetSystemMetrics(1 /*SM_CYSCREEN*/); }
      if (ww > 0 && wh > 0) gp3_widen_modes(ww, wh);
      if (GetPrivateProfileIntA("video", "hor_plus", 1, g_ini_path)) gp3_hor_plus((int)GetPrivateProfileIntA("video", "fov_percent", 100, g_ini_path));
      }
    if (GetPrivateProfileIntA("game", "str_guard", 1, g_ini_path)) gp3_fix_string_table();
    o_ExitProcess = iat_hook(exe, "kernel32.dll", "ExitProcess", (void *)h_ExitProcess);
    if (!o_ExitProcess) LOG("note: ExitProcess hook not installed");
    o_PeekMessageA = iat_hook(exe, "user32.dll", "PeekMessageA", (void *)h_PeekMessageA);
    o_GetMessageA = iat_hook(exe, "user32.dll", "GetMessageA", (void *)h_GetMessageA);
    o_SendMessageA = iat_hook(exe, "user32.dll", "SendMessageA", (void *)h_SendMessageA);
    o_ClientToScreen = iat_hook(exe, "user32.dll", "ClientToScreen", (void *)h_ClientToScreen);
    o_SetWindowLongA = iat_hook(exe, "user32.dll", "SetWindowLongA", (void *)h_SetWindowLongA);
    o_GetWindowRect = iat_hook(exe, "user32.dll", "GetWindowRect", (void *)h_GetWindowRect);
    o_GetClientRect = iat_hook(exe, "user32.dll", "GetClientRect", (void *)h_GetClientRect);
    o_ShowCursor = iat_hook(exe, "user32.dll", "ShowCursor", (void *)h_ShowCursor);
    if (!o_ClientToScreen) LOG("note: ClientToScreen hook not installed");
}

/* ------------------------------------------------------------------ */
extern void ddraw_register(void);
extern void win_platform_init(void);

BOOL WINAPI DllMain(HMODULE h, DWORD reason, void *res) {
    (void)res;
    if (reason == 0) { LOG("process exiting normally (DLL detach)"); return 1; }
    if (reason != 1) return 1;
    DisableThreadLibraryCalls(h);
    char exe[1024];
    GetModuleFileNameA(0, exe, sizeof exe);
    char *sl = strrchr(exe, '\\'); if (sl) *sl = 0;
    gp3_snprintf(g_game_dir, sizeof g_game_dir, "%s", exe);
    cfg_load();
    os_mutex_init(&g_logmx); g_logmx_ok = 1;
    if (g_log_level > 0) {
        char lp[1100]; gp3_snprintf(lp, sizeof lp, "%s\\gp3rt.log", g_game_dir);
        g_log = fopen(lp, "w");
    }
    LOG("gp3rt ddraw.dll %s - game dir %s", GP3RT_VERSION, g_game_dir);
    LOG("config: %s, renderer %s, internal scale %d (0 = auto), filtering %d, aniso %d, vsync %d, virtual CD %d, DirectPlay %d, overlay locks %d, spray %d%%, exception-free %d",
        g_wcfg.windowed ? "window" : "fullscreen", g_wcfg.software ? "software" : "opengl", g_wcfg.internal_scale,
        g_wcfg.filtering, g_wcfg.anisotropy, g_wcfg.vsync, g_wcfg.virtual_cd, g_wcfg.multiplayer, g_overlay_locks, g_spray_opacity, g_nofault);
    memcpy(g_ini_path_ext, g_ini_path, sizeof g_ini_path);
    g_fast_exit = (int)GetPrivateProfileIntA("advanced", "fast_exit", 1, g_ini_path);
    win_fix_affinity(0);
    if (GetPrivateProfileIntA("advanced", "close_leftover_instances", 1, g_ini_path) && !dbg_present()) kill_stale_instances();
    { extern void gpu_prefer(const char *ini); gpu_prefer(g_ini_path); }
    install_hooks();
    win_platform_init();
    { extern void win_watchdog_start(void); win_watchdog_start(); }
    ddraw_register();
    return 1;
}

/* ------------------------------------------------------------------ */
/* exported DirectDraw entry points                                     */
typedef LONG (WINAPI *ddce_t)(const void *, void **, const void *, void *);
typedef LONG (WINAPI *ddenumex_t)(void *, void *, DWORD);
typedef LONG (WINAPI *ddenum_t)(void *, void *);
LONG WINAPI DirectDrawCreateEx(const void *guid, void **out, const void *iid, void *outer) {
    return ((ddce_t)rt_find_export("ddraw", "DirectDrawCreateEx", 0))(guid, out, iid, outer);
}
LONG WINAPI DirectDrawEnumerateExA(void *cb, void *ctx, DWORD fl) {
    return ((ddenumex_t)rt_find_export("ddraw", "DirectDrawEnumerateExA", 0))(cb, ctx, fl);
}
LONG WINAPI DirectDrawEnumerateA(void *cb, void *ctx) {
    return ((ddenum_t)rt_find_export("ddraw", "DirectDrawEnumerateA", 0))(cb, ctx);
}
LONG WINAPI DirectDrawCreate(const void *guid, void **out, void *outer) {
    (void)guid; (void)outer; if (out) *out = NULL;
    LOG("DirectDrawCreate (legacy interface) requested - unsupported");
    return (LONG)0x80004001;
}
LONG WINAPI DirectDrawCreateClipper(DWORD f, void **out, void *outer) { (void)f; (void)outer; if (out) *out = NULL; return (LONG)0x80004001; }
LONG WINAPI DllCanUnloadNow(void) { return 1; }
LONG WINAPI DllGetClassObject(const void *a, const void *b, void **c) { (void)a; (void)b; if (c) *c = NULL; return (LONG)0x80040111; }
