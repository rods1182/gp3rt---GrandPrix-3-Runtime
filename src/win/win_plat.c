/* Windows platform layer of gp3rt ddraw.dll:
 *  - presentation into the game window (OpenGL 3.3, or GDI for the software renderer)
 *  - borderless fullscreen / resizable window without ever changing the desktop display mode
 *  - mouse coordinates translated back to the game's resolution
 *  - DirectDraw GetDC through real GDI DIB sections
 *  - page-aligned surface memory + vectored exception handler (lazy render-target readback) */
#include "win32.h"
#include "../plat.h"
#include "../ddraw_int.h"
#include "../rdr.h"
#include "../rdr_gl.h"
#include "win_cfg.h"
#include "../osdep.h"

static HWND g_hwnd;
static inline int ckp(void *p) { return p != 0; }
static WNDPROC g_orig_proc;
static int g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 16;
static int g_in_layout; static int g_in_sizemove;
static WRECT g_want;      /* desired window rect (screen) */
static HDC g_gl_dc; static HGLRC g_gl_rc;
/* OpenGL draws into a child window of our own covering the game window's client area
 * (class with CS_OWNDC). Drawing straight into GP3's window broke the window mode:
 * after the window left the full-screen position the NVIDIA driver kept showing the
 * picture shifted by the window's position. The child is disabled and transparent to
 * hit-testing, so mouse and keyboard still go to the game window. */
static HWND g_gl_wnd;
static HWND gl_surface(void) { return g_gl_wnd ? g_gl_wnd : g_hwnd; }
static LRESULT WINAPI gl_child_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == 0x84) return -1;   /* WM_NCHITTEST: HTTRANSPARENT */
    if (m == 0x14) return 1;    /* WM_ERASEBKGND */
    if (m == 0x0F) { /* WM_PAINT: validate, the next frame paints */ return DefWindowProcA(h, m, w, l); }
    return DefWindowProcA(h, m, w, l);
}
static void gl_child_fit(void) {
    if (!g_gl_wnd) return;
    WRECT r; GetClientRect(g_hwnd, &r);
    MoveWindow(g_gl_wnd, 0, 0, r.right, r.bottom, 0);
}
static void gl_child_create(void) {
    extern char g_ini_path_ext[];
    if (!GetPrivateProfileIntA("advanced", "gl_child_window", 0, g_ini_path_ext)) return;
    if (GetWindowThreadProcessId(g_hwnd, NULL) != GetCurrentThreadId()) { LOG("OpenGL: drawing into the game window (renderer started on another thread)"); return; }
    static int reg;
    HINSTANCE inst = (HINSTANCE)(uintptr_t)GetModuleHandleA(NULL);
    if (!reg) {
        WNDCLASSA_ wc; memset(&wc, 0, sizeof wc);
        wc.style = 0x20 /* CS_OWNDC */; wc.proc = gl_child_proc; wc.inst = inst; wc.name = "GP3RT_GL";
        reg = RegisterClassA(&wc) != 0;
        if (!reg) { LOG("OpenGL: child window class not registered: drawing into the game window"); return; }
    }
    WRECT r; GetClientRect(g_hwnd, &r);
    SetWindowLongA(g_hwnd, GWL_STYLE, (LONG)((DWORD)GetWindowLongA(g_hwnd, GWL_STYLE) | 0x02000000u /* WS_CLIPCHILDREN */));
    g_gl_wnd = CreateWindowExA(0, "GP3RT_GL", "", 0x40000000u | 0x10000000u | 0x08000000u | 0x04000000u /* CHILD VISIBLE DISABLED CLIPSIBLINGS */,
                               0, 0, r.right, r.bottom, g_hwnd, 0, inst, NULL);
    LOG("OpenGL: %s", g_gl_wnd ? "drawing into a child window of the game window" : "child window failed: drawing into the game window");
}
static HMODULE g_opengl32;

/* ------------------------------------------------------------------ */
/* presentation rectangle inside the client area                        */
static void present_rect(int cw, int ch, int *vx, int *vy, int *vw, int *vh) {
    if (g_wcfg.stretch || g_mode_w <= 0 || g_mode_h <= 0) { *vx = 0; *vy = 0; *vw = cw; *vh = ch; return; }
    double sa = (double)g_mode_w / g_mode_h, wa = (double)cw / (ch ? ch : 1);
    int w = cw, h = ch;
    if (wa > sa) w = (int)(ch * sa + 0.5); else h = (int)(cw / sa + 0.5);
    *vx = (cw - w) / 2; *vy = (ch - h) / 2; *vw = w; *vh = h;
}

/* ------------------------------------------------------------------ */
/* window layout                                                         */
extern char g_ini_path_ext[];
static void apply_layout(void) {
    if (!g_hwnd) return;
    MONITORINFO mi; mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    g_in_layout = 1;
    if (!g_wcfg.windowed) {
        g_want = mi.rcMonitor;
        /* one pixel taller than the monitor: a borderless OpenGL window exactly covering
         * the screen is switched by NVIDIA drivers to "fullscreen exclusive" presentation,
         * which breaks Alt+Tab, window/fullscreen switching and focus on notebooks */
        if (!g_wcfg.software && GetPrivateProfileIntA("advanced", "avoid_exclusive_fullscreen", 1, g_ini_path_ext)) g_want.bottom += 1;
        SetWindowLongA(g_hwnd, GWL_STYLE, (LONG)(WS_POPUP | WS_VISIBLE | (g_gl_wnd ? 0x02000000u : 0)));
        SetWindowLongA(g_hwnd, GWL_EXSTYLE, 0);
    } else {
        int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
        /* the window keeps one size for the whole session, whatever the game's mode (menu
         * 800x600, race 1536x864...): GP3 does not survive its window being resized in the
         * middle of a mode switch. The picture is letterboxed inside, as in full screen.
         * Size: [video] window_size = WIDTHxHEIGHT of the picture area, or by default 80% of
         * the screen with the screen's proportions. */
        static int cw, ch;
        if (!cw) {
            char b[32] = ""; GetPrivateProfileStringA("video", "window_size", "", b, sizeof b, g_ini_path_ext);
            int w = 0, hh = 0; const char *q = b; while (*q >= '0' && *q <= '9') w = w * 10 + *q++ - '0';
            if (*q == 'x' || *q == 'X') { q++; while (*q >= '0' && *q <= '9') hh = hh * 10 + *q++ - '0'; }
            if (w >= 320 && hh >= 200) { cw = w; ch = hh; }
            else {
                int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
                cw = aw * 8 / 10; ch = mh ? (int)((double)cw * mh / mw + 0.5) : cw * 9 / 16;
                if (ch > ah * 85 / 100) { ch = ah * 85 / 100; cw = (int)((double)ch * mw / mh + 0.5); }
            }
        }
        DWORD style = 0x00CA0000u /* CAPTION SYSMENU MINIMIZEBOX: no resizing, no maximize */ | WS_VISIBLE;
        WRECT r = { 0, 0, cw, ch };
        AdjustWindowRectEx(&r, style, 0, 0);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        g_want.left = mi.rcWork.left + (aw - ww) / 2; g_want.top = mi.rcWork.top + (ah - wh) / 2;
        if (g_want.top < mi.rcWork.top) g_want.top = mi.rcWork.top;
        g_want.right = g_want.left + ww; g_want.bottom = g_want.top + wh;
        SetWindowLongA(g_hwnd, GWL_STYLE, (LONG)(style | (g_gl_wnd ? 0x02000000u : 0)));
    }
    SetWindowPos(g_hwnd, HWND_TOP, g_want.left, g_want.top, g_want.right - g_want.left, g_want.bottom - g_want.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    gl_child_fit();
    g_in_layout = 0;
    LOG("window layout: %s %dx%d at %d,%d (game %dx%d)", g_wcfg.windowed ? "window" : "fullscreen",
        g_want.right - g_want.left, g_want.bottom - g_want.top, g_want.left, g_want.top, g_mode_w, g_mode_h);
}

static LPARAM map_mouse(LPARAM l) {
    WRECT cr; GetClientRect(g_hwnd, &cr);
    int vx, vy, vw, vh; present_rect(cr.right, cr.bottom, &vx, &vy, &vw, &vh);
    int x = (short)(l & 0xffff), y = (short)((l >> 16) & 0xffff);
    int gx = vw ? (x - vx) * g_mode_w / vw : x, gy = vh ? (y - vy) * g_mode_h / vh : y;
    if (gx < 0) gx = 0; if (gy < 0) gy = 0;
    if (gx >= g_mode_w) gx = g_mode_w - 1; if (gy >= g_mode_h) gy = g_mode_h - 1;
    return (LPARAM)(((uint32_t)gy << 16) | ((uint32_t)gx & 0xffff));
}

/* ---- OpenGL context hand-over between threads (see d3d.c) ---- */
static volatile unsigned g_ctx_tid;          /* thread the context is current on (0 = none) */
static int g_emulate_ctx;                    /* [debug] thread_bound with the software renderer */
static os_mutex_t g_ctx_mx; static int g_ctx_mx_init;
/* The window thread keeps the context between messages (releasing it after every
 * message made each menu frame of GP3 2000 call wglMakeCurrent twice, which the
 * NVIDIA driver can block on until Windows considers the window hung). Another
 * thread that needs it raises g_ctx_want and posts CTX_MSG to wake the window
 * thread, which then lets go. */
#define CTX_MSG 0x83A7   /* WM_APP + 0x3A7 */
static volatile int g_ctx_want; static volatile DWORD g_ctx_post_tick;
static int ctx_acquire(void) {
    unsigned me = GetCurrentThreadId();
    if (g_ctx_tid == me) return 1;
    if (!g_ctx_mx_init) return 1;
    os_mutex_lock(&g_ctx_mx);
    int ok = 0;
    unsigned holder = g_ctx_tid;
    if (holder) {
        g_ctx_want = 1;
        DWORD now = GetTickCount();
        if (g_hwnd && now - g_ctx_post_tick > 20 && holder == GetWindowThreadProcessId(g_hwnd, NULL)) { g_ctx_post_tick = now; PostMessageA(g_hwnd, CTX_MSG, 0, 0); }
    }
    if (!g_ctx_tid) {
        { extern uint64_t g_t_ctx; uint64_t t0 = rt_now_us(); ok = g_emulate_ctx ? 1 : wglMakeCurrent(g_gl_dc, g_gl_rc); g_t_ctx += rt_now_us() - t0; }
        if (ok) {
            static unsigned last;
            g_ctx_tid = me;
            extern uint32_t g_ctx_moves;
            if (last && last != me) g_ctx_moves++;   /* count real hand-overs between threads */
            last = me;
            static int n; extern void *g_rdr_caller;
            if (g_ctx_moves && n < (g_log_level >= 2 ? 400 : 20)) { n++; LOG("render context now on thread %u (for %p)", me, g_rdr_caller); }
        }
    }
    os_mutex_unlock(&g_ctx_mx);
    return ok;
}
void win_ctx_release(void) {
    if (!g_ctx_mx_init || g_ctx_tid != GetCurrentThreadId()) return;
    os_mutex_lock(&g_ctx_mx);
    { extern uint64_t g_t_ctx; uint64_t t0 = rt_now_us(); if (!g_emulate_ctx) wglMakeCurrent(0, 0); g_t_ctx += rt_now_us() - t0; }     /* also flushes pending GL work */
    g_ctx_tid = 0; g_ctx_want = 0;
    os_mutex_unlock(&g_ctx_mx);
}
void win_ctx_release_if_wanted(void) { if (g_ctx_want) win_ctx_release(); }
static int ctx_held_elsewhere(void) { unsigned t = g_ctx_tid; return t && t != GetCurrentThreadId(); }
static void ctx_setup(int emulate) {
    extern int g_rdr_thread_bound; extern int (*g_rdr_acquire)(void); extern int (*g_rdr_held_elsewhere)(void);
    g_rdr_held_elsewhere = ctx_held_elsewhere;
    os_mutex_init(&g_ctx_mx); g_ctx_mx_init = 1;
    g_emulate_ctx = emulate;
    g_ctx_tid = GetCurrentThreadId();           /* created (current) on this thread */
    g_rdr_thread_bound = 1; g_rdr_acquire = ctx_acquire;
}

static volatile int g_shot_pending;
static void shot_save(const uint32_t *xrgb, int w, int h, int pitch);
static LRESULT WINAPI sub_proc2(HWND h, UINT m, WPARAM w, LPARAM l);
static volatile int g_in_proc; static volatile DWORD g_proc_enter_tick; static volatile UINT g_proc_msg;
static volatile int g_ghost_seen;
static LRESULT WINAPI sub_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_in_proc++ == 0) { g_proc_enter_tick = GetTickCount(); g_proc_msg = m; }
    /* a window message may borrow the render context (GP3 switches modes and draws
     * loading screens on its window thread): give it back when the message is done */
    unsigned me = GetCurrentThreadId();
    int held = g_ctx_tid == me;
    if (m == CTX_MSG) { win_ctx_release_if_wanted(); g_in_proc--; return 0; }
    LRESULT r = sub_proc2(h, m, w, l);
    if (!held && g_ctx_tid == me) win_ctx_release_if_wanted();
    g_in_proc--;
    return r;
}
/* ---- focus guard + input diagnostics ----
 * DirectInput (keyboard/mouse in GP3) only delivers input to the foreground window.
 * On notebooks with two GPUs the OpenGL driver can create helper windows while it
 * initialises and take the foreground away from the game: no keyboard, no mouse.
 * The game window is brought back when a window of our own process (or, during
 * the first seconds, any window) has the foreground. */
static DWORD g_t_start, g_fg_last_check, g_in_last_log;
static volatile uint32_t g_n_keys, g_n_mouse, g_n_msgs;
static HWND g_fg_logged;
static uint32_t g_swap_max_us, g_swap_sum_us, g_swap_n;
static int g_fg_fixes;
static void force_foreground(void) {
    DWORD fgt = GetWindowThreadProcessId(GetForegroundWindow(), NULL), me = GetCurrentThreadId();
    int att = fgt && fgt != me && AttachThreadInput(me, fgt, TRUE);
    BringWindowToTop(g_hwnd); SetForegroundWindow(g_hwnd); SetFocus(g_hwnd);
    if (att) AttachThreadInput(me, fgt, FALSE);
}
static void focus_guard(void) {
    DWORD now = GetTickCount();
    if (!g_t_start) g_t_start = now;
    if (now - g_fg_last_check < 250 || !g_hwnd) return;
    g_fg_last_check = now;
    HWND fg = GetForegroundWindow();
    if (fg != g_hwnd && fg != g_fg_logged) {
        g_fg_logged = fg;
        char cls[64] = "?"; DWORD pid = 0;
        if (fg) { GetClassNameA(fg, cls, sizeof cls); GetWindowThreadProcessId(fg, &pid); }
        LOG("focus: foreground window is %x class '%s' (process %u%s)", (unsigned)fg, cls, (unsigned)pid, pid == GetCurrentProcessId() ? ", this game" : "");
        if (!strcmp(cls, "Ghost")) g_ghost_seen = 1;   /* Windows replaced our window: it considers it hung */
        if (!g_wcfg.windowed && g_fg_fixes < 20 && (pid == GetCurrentProcessId() || now - g_t_start < 15000 || !fg)) {
            g_fg_fixes++;
            force_foreground();
            LOG("focus: game window brought back to the foreground -> %s", GetForegroundWindow() == g_hwnd ? "ok" : "refused by Windows");
            g_fg_logged = 0;
        }
    } else if (fg == g_hwnd) g_fg_logged = 0;
    if (now - g_in_last_log >= 10000) {
        if (g_in_last_log) LOG("input/10s: %u messages, %u keys, %u mouse; swap avg %.1f ms max %.1f ms; foreground %s",
                               g_n_msgs, g_n_keys, g_n_mouse, g_swap_n ? g_swap_sum_us / 1000.0 / g_swap_n : 0.0, g_swap_max_us / 1000.0,
                               fg == g_hwnd ? "game" : "OTHER");
        g_in_last_log = now; g_n_msgs = g_n_keys = g_n_mouse = 0; g_swap_max_us = g_swap_sum_us = g_swap_n = 0;
    }
}

DLLIMP HANDLE WINAPI CreateThread(void *, size_t, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
static DWORD WINAPI close_later(void *p) {
    (void)p; Sleep(3000);
    extern void win_clean_exit(UINT); LOG("window closed by the player"); win_clean_exit(0);
    return 0;
}
static LRESULT WINAPI sub_proc2(HWND h, UINT m, WPARAM w, LPARAM l) {
    g_n_msgs++;
    if (m == 0x100 || m == 0x104) g_n_keys++;
    else if (m >= 0x200 && m <= 0x20A) g_n_mouse++;
    else if (m == 0x1C || m == 0x06 || m == 0x07 || m == 0x08) LOG("focus: %s %s", m == 0x1C ? "WM_ACTIVATEAPP" : m == 0x06 ? "WM_ACTIVATE" : m == 0x07 ? "WM_SETFOCUS" : "WM_KILLFOCUS",
                                                               (m == 0x07 || ((m == 0x1C || m == 0x06) && (w & 0xffff))) ? "(gained)" : "(lost)");
    /* GP3 was written for exclusive full screen and never expects its window to change size
     * or place. In window mode every game mode switch (menu 800x600 -> race) resizes the
     * window from inside SetDisplayMode, and the game's own handling of those messages in the
     * middle of its mode switch broke the start of a race. Our own layout changes are not
     * passed on to the game. */
    if (g_in_layout && (m == 0x03 /* WM_MOVE */ || m == 0x05 /* WM_SIZE */ || m == 0x47 /* WM_WINDOWPOSCHANGED */ || m == 0x24 /* WM_GETMINMAXINFO */)) {
        static int n; if (n < 6) { n++; LOG("window message %x from our layout change kept from the game", m); }
        if (m == 0x05) gl_child_fit();
        return DefWindowProcA(h, m, w, l);
    }
    /* the window frame belongs to Windows: GP3 swallows the non-client and system-command
     * messages (it never had a frame), so moving the window by its title bar never started */
    if (g_wcfg.windowed && ((m >= 0xA0 && m <= 0xAD) /* WM_NCMOUSEMOVE..WM_NCXBUTTON */ || m == 0x84 /* WM_NCHITTEST */ ||
                            (m == 0x112 /* WM_SYSCOMMAND */ && ((w & 0xfff0) == 0xF010 /* SC_MOVE */ || (w & 0xfff0) == 0xF020 /* SC_MINIMIZE */ ||
                                                                (w & 0xfff0) == 0xF120 /* SC_RESTORE */ || (w & 0xfff0) == 0xF060 /* SC_CLOSE */))))
        return DefWindowProcA(h, m, w, l);
    switch (m) {
    case 0x46: /* WM_WINDOWPOSCHANGING: keep our layout (the game moves and resizes its window on mode switches) */
        if (!g_in_layout && !g_in_sizemove && !IsIconic(h)) {
            WINDOWPOS *wp = (WINDOWPOS *)l;
            if (!(wp->flags & 0x2) && wp->x > -30000) { wp->x = g_want.left; wp->y = g_want.top; }
            if (!(wp->flags & 0x1)) { wp->cx = g_want.right - g_want.left; wp->cy = g_want.bottom - g_want.top; }
        }
        return DefWindowProcA(h, m, w, l);
    case 0x231: g_in_sizemove = 1; return DefWindowProcA(h, m, w, l);   /* WM_ENTERSIZEMOVE: the player drags the window */
    case 0x232: g_in_sizemove = 0; if (g_wcfg.windowed && !IsIconic(h)) GetWindowRect(h, &g_want); return DefWindowProcA(h, m, w, l);
    case 0x20: { /* WM_SETCURSOR: hidden over the picture when the game hides it, normal on the frame */
        extern volatile int g_game_cursor;
        if ((l & 0xffff) == 1 /* HTCLIENT */) { if (g_game_cursor < 0) { SetCursor(0); return 1; } SetCursor(LoadCursorA(0, (const char *)32512)); return 1; }
        return DefWindowProcA(h, m, w, l);
    }
    case 0x10: { /* WM_CLOSE (close button): the game gets it; if it is still running 3 s later, close it cleanly */
        static int closing;
        if (!closing) { closing = 1; DWORD id; CreateThread(NULL, 0, close_later, NULL, 0, &id); }
        break;
    }
    case 0x200: case 0x201: case 0x202: case 0x203: case 0x204: case 0x205: case 0x206: case 0x207: case 0x208: case 0x209:
        l = map_mouse(l);
        break;
    case 0x101: case 0x105: /* WM_KEYUP / WM_SYSKEYUP: Print Screen takes a screenshot; Ctrl+Print Screen also a frame dump */
        if (w == 0x2C) {
            g_shot_pending = 1;
            if (GetAsyncKeyState(0x11) < 0) { extern void rdr_framedump_arm(void); rdr_framedump_arm(); LOG("frame dump requested (Ctrl+Print Screen)"); }
            return 0;
        }
        break;
    case 0x100: /* WM_KEYDOWN: Ctrl+F11 = frame dump (Print Screen may be taken by GPxPatch) */
        if (w == 0x7A && GetAsyncKeyState(0x11) < 0) { extern void rdr_framedump_arm(void); rdr_framedump_arm(); g_shot_pending = 1; LOG("frame dump requested (Ctrl+F11)"); return 0; }
        break;
    case 0x104: /* WM_SYSKEYDOWN: Alt+Enter toggles window/fullscreen */
        if (w == 0x0d && (l & (1 << 29))) { g_wcfg.windowed = !g_wcfg.windowed; apply_layout(); return 0; }
        break;
    case 0x106: /* WM_SYSCHAR: swallow the beep of Alt+Enter */
        if (w == 0x0d) return 0;
        break;
    case 0x05: gl_child_fit(); break;   /* WM_SIZE: the OpenGL child follows the client area */
    case 0x14: return 1; /* WM_ERASEBKGND: we paint everything */
    }
    return CallWindowProcA(g_orig_proc, h, m, w, l);
}

void plat_set_window(void *hwnd_, unsigned flags) {
    HWND hwnd = (HWND)(uintptr_t)hwnd_;
    if (!hwnd || hwnd == g_hwnd) return;
    g_hwnd = hwnd;
    if (!g_orig_proc) {
        g_orig_proc = (WNDPROC)(uintptr_t)GetWindowLongA(g_hwnd, GWL_WNDPROC);
        SetWindowLongA(g_hwnd, GWL_WNDPROC, (LONG)(uintptr_t)sub_proc);
    }
    apply_layout();
    /* exclusive DirectDraw made the game window the foreground window; a borderless
     * window started by a launcher (e.g. GPxPatch) would otherwise stay behind it
     * without keyboard focus */
    if (flags & 0x10 /* DDSCL_FULLSCREEN */ || !g_wcfg.windowed) {
        BringWindowToTop(g_hwnd);
        SetForegroundWindow(g_hwnd);
        SetFocus(g_hwnd);
        LOG("game window brought to the foreground (foreground now %x)", (unsigned)GetForegroundWindow());
    }
}
void plat_set_mode(int w, int h, int bpp) {
    int changed = w != g_mode_w || h != g_mode_h;
    g_mode_w = w; g_mode_h = h; g_mode_bpp = bpp;
    LOG("game display mode %dx%dx%d (desktop mode untouched)", w, h, bpp);
    (void)changed;
    if (g_hwnd && !g_wcfg.windowed) apply_layout();
    else if (g_hwnd && g_wcfg.windowed && !(GetWindowLongA(g_hwnd, GWL_STYLE) & 0x00C00000u /* WS_CAPTION */)) apply_layout();
}
HWND win_game_hwnd(void) { return g_hwnd; }
void plat_client_size(int *w, int *h) { *w = g_mode_w; *h = g_mode_h; }
void plat_show_cursor(int show) { (void)show; }
void plat_message_box(const char *cap, const char *text) { (void)cap; (void)text; }
void plat_screenshot(const char *name) { (void)name; }
void plat_pump(void) {}

/* software renderer output: GDI */
void plat_present(const uint32_t *xrgb, int w, int h, int pitch_px) {
    if (!g_hwnd) return;
    focus_guard();
    HDC dc = GetDC(g_hwnd);
    WRECT cr; GetClientRect(g_hwnd, &cr);
    int vx, vy, vw, vh; present_rect(cr.right, cr.bottom, &vx, &vy, &vw, &vh);
    BITMAPINFO256 bi; memset(&bi, 0, sizeof bi.h);
    bi.h.biSize = 40; bi.h.biWidth = pitch_px; bi.h.biHeight = -h; bi.h.biPlanes = 1; bi.h.biBitCount = 32;
    if (vx > 0) { PatBlt(dc, 0, 0, vx, cr.bottom, 0x42); PatBlt(dc, vx + vw, 0, cr.right - vx - vw, cr.bottom, 0x42); }
    if (vy > 0) { PatBlt(dc, 0, 0, cr.right, vy, 0x42); PatBlt(dc, 0, vy + vh, cr.right, cr.bottom - vy - vh, 0x42); }
    StretchDIBits(dc, vx, vy, vw, vh, 0, 0, w, h, xrgb, &bi, 0, 0x00CC0020);
    if (g_shot_pending) { g_shot_pending = 0; shot_save(xrgb, w, h, pitch_px); }
    ReleaseDC(g_hwnd, dc);
}

/* ------------------------------------------------------------------ */
/* OpenGL                                                                */
static void *gl_getproc(const char *n) {
    void *p = wglGetProcAddress(n);
    if (!p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) {
        if (!g_opengl32) g_opengl32 = LoadLibraryA("opengl32.dll");
        p = GetProcAddress(g_opengl32, n);
    }
    (void)ckp;
    return p;
}
/* Print Screen: the next presented frame is saved as a PNG in <game>\\screens */
DLLIMP BOOL WINAPI CreateDirectoryA(const char *, void *);
static void shot_path(char *p, size_t n) {
    extern char g_game_dir[];
    char dir[1100]; gp3_snprintf(dir, sizeof dir, "%s\\screens", g_game_dir);
    CreateDirectoryA(dir, NULL);
    for (int i = 1; i < 100000; i++) {
        gp3_snprintf(p, n, "%s\\gp3rt_%05d.png", dir, i);
        if (GetFileAttributesA(p) == 0xFFFFFFFFu) return;
    }
}
static void shot_save(const uint32_t *xrgb, int w, int h, int pitch) {
    char p[1200]; shot_path(p, sizeof p);
    if (write_png(p, xrgb, w, h, pitch)) LOG("screenshot saved: %s (%dx%d)", p, w, h);
    else WARN("screenshot failed: %s", p);
}
static void gl_swap(void *c) {
    (void)c;
    focus_guard();
    if (g_shot_pending) {
        g_shot_pending = 0;
        WRECT r; GetClientRect(gl_surface(), &r);
        int w = r.right, h = r.bottom;
        uint32_t *buf = malloc((size_t)w * h * 4);
        extern int rdr_gl_read_window(uint32_t *argb, int w, int h);
        if (buf) { rdr_gl_read_window(buf, w, h); shot_save(buf, w, h, w); free(buf); }
    }
    { uint64_t t0 = rt_now_us(); SwapBuffers(g_gl_dc); uint32_t d = (uint32_t)(rt_now_us() - t0);
      g_swap_sum_us += d; g_swap_n++; if (d > g_swap_max_us) g_swap_max_us = d; }
}
/* client size for the frame about to be drawn. When it changes (Alt+Enter, resizing),
 * the context is bound to the window again so drivers (NVIDIA) resize their back
 * buffer; otherwise the old full-screen-sized buffer is shown cropped in the window. */
static void gl_size(void *c, int *w, int *h) {
    (void)c; HWND sw = gl_surface(); WRECT r; GetClientRect(sw, &r); *w = r.right; *h = r.bottom;
    WPOINT o = { 0, 0 }; ClientToScreen(sw, &o);
    static int lw, lh, lx, ly;
    if ((r.right != lw || r.bottom != lh || o.x != lx || o.y != ly) && g_gl_rc) {
        if (lw) {
            /* GP3's window class has no CS_OWNDC: the DC from GetDC keeps the screen origin the
             * window had when it was taken. After the window moves (windowed mode is centred
             * again on every game mode change) the picture would be drawn shifted by the old
             * position, so take a fresh DC for the new place and bind the context to it. */
            HDC nd = g_gl_wnd ? g_gl_dc : GetDC(sw), od = g_gl_dc;   /* CS_OWNDC child: its DC never goes stale */
            wglMakeCurrent(0, 0);
            if (nd && wglMakeCurrent(nd, g_gl_rc)) { g_gl_dc = nd; if (od && od != nd) ReleaseDC(sw, od); }
            else { if (nd && nd != od) ReleaseDC(sw, nd); wglMakeCurrent(g_gl_dc, g_gl_rc); }
            LOG("window client area %dx%d at %d,%d -> %dx%d at %d,%d: render target rebound", lw, lh, lx, ly, (int)r.right, (int)r.bottom, (int)o.x, (int)o.y);
        }
        lw = r.right; lh = r.bottom; lx = o.x; ly = o.y;
    }
}
static void gl_log(const char *m) { LOG("%s", m); }

static rbackend_t *win_backend_create(void) {
    if (g_wcfg.software) {
        LOG("renderer: software (gp3rt.ini)");
        if (g_wcfg.emulate_ctx) { LOG("debug: emulating thread-bound render context"); ctx_setup(1); }
        return NULL;
    }
    if (!g_hwnd) { WARN("renderer requested before the game window is known: using software"); return NULL; }
    gl_child_create();
    g_gl_dc = GetDC(gl_surface());
    PIXELFORMATDESCRIPTOR pfd; memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = 0x4 | 0x20 | 0x1;    /* DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER */
    pfd.iPixelType = 0; pfd.cColorBits = 32; pfd.cAlphaBits = 8; pfd.iLayerType = 0;
    int pf = ChoosePixelFormat(g_gl_dc, &pfd);
    if (!pf || !SetPixelFormat(g_gl_dc, pf, &pfd)) { WARN("OpenGL: no pixel format, using software"); return NULL; }
    HGLRC legacy = wglCreateContext(g_gl_dc);
    if (!legacy || !wglMakeCurrent(g_gl_dc, legacy)) { WARN("OpenGL: cannot create context, using software"); return NULL; }
    g_gl_rc = legacy;
    typedef HGLRC (WINAPI *cca_t)(HDC, HGLRC, const int *);
    cca_t cca = (cca_t)wglGetProcAddress("wglCreateContextAttribsARB");
    if (cca) {
        const int attr[] = { 0x2091, 3, 0x2092, 3, 0x9126, 0x1 /* core */, 0 };
        HGLRC core = cca(g_gl_dc, 0, attr);
        if (core && wglMakeCurrent(g_gl_dc, core)) { wglDeleteContext(legacy); g_gl_rc = core; }
        else wglMakeCurrent(g_gl_dc, legacy);
    }
    typedef BOOL (WINAPI *swi_t)(int);
    swi_t swi = (swi_t)wglGetProcAddress("wglSwapIntervalEXT");
    if (swi) swi(g_wcfg.vsync ? 1 : 0);
    rdr_gl_host_t host = { NULL, gl_getproc, gl_swap, gl_size, gl_log, NULL };
    int scale = g_wcfg.internal_scale, aniso = g_wcfg.anisotropy;
    { extern void gpu_after_context(void *(*)(const char *), int *, int *); gpu_after_context(gl_getproc, &scale, &aniso); }
    rdr_gl_options_t opt = { scale, aniso, g_wcfg.filtering, 1 };
    rbackend_t *b = rdr_gl_init(&host, &opt);
    if (!b) WARN("OpenGL 3.3 not available: using software");
    else {
        extern int g_emulate_vblank;
        g_emulate_vblank = 0;      /* SwapBuffers paces the frames */
        ctx_setup(0);              /* the WGL context is current on one thread at a time */
        if (!g_wcfg.windowed && GetForegroundWindow() != g_hwnd) {   /* the driver may have taken it while loading */
            force_foreground();
            LOG("focus: foreground re-asserted after OpenGL start -> %s", GetForegroundWindow() == g_hwnd ? "ok" : "refused by Windows");
        }
    }
    return b;
}

/* ------------------------------------------------------------------ */
/* GetDC on DirectDraw surfaces: real GDI DIB sections                   */
typedef struct { HDC dc; HBITMAP dib; HGDIOBJ old; uint8_t *dibbits; void *bits; int w, h, pitch, bpp, stride; } wdc_t;
static wdc_t g_dcs[16];
uint32_t gdi_surface_dc(void *bits, int w, int h, int pitch, int bpp, const uint32_t *pal, const uint32_t masks[3]) {
    int i; for (i = 0; i < 16 && g_dcs[i].dc; i++) {}
    if (i == 16) return 0;
    BITMAPINFO256 bi; memset(&bi, 0, sizeof bi);
    bi.h.biSize = 40; bi.h.biWidth = w; bi.h.biHeight = -h; bi.h.biPlanes = 1; bi.h.biBitCount = (WORD)bpp;
    if (bpp == 8) { bi.h.biClrUsed = 256; for (int k = 0; k < 256; k++) bi.colors[k] = pal ? pal[k] : (uint32_t)k * 0x010101u; }
    else if (bpp == 16) { bi.h.biCompression = 3; bi.colors[0] = masks[0]; bi.colors[1] = masks[1]; bi.colors[2] = masks[2]; }
    wdc_t *d = &g_dcs[i];
    d->dc = CreateCompatibleDC(0);
    void *db = NULL;
    d->dib = CreateDIBSection(d->dc, &bi, 0, &db, 0, 0);
    if (!d->dib) { DeleteDC(d->dc); d->dc = 0; return 0; }
    d->dibbits = db; d->bits = bits; d->w = w; d->h = h; d->pitch = pitch; d->bpp = bpp;
    d->stride = ((w * bpp + 31) / 32) * 4;
    for (int y = 0; y < h; y++) memcpy(d->dibbits + (size_t)y * d->stride, (uint8_t *)bits + (size_t)y * pitch, (size_t)w * bpp / 8);
    d->old = SelectObject(d->dc, d->dib);
    return (uint32_t)(uintptr_t)d->dc;
}
void gdi_free_dc(uint32_t hdc) {
    for (int i = 0; i < 16; i++) if ((uint32_t)(uintptr_t)g_dcs[i].dc == hdc) {
        wdc_t *d = &g_dcs[i];
        GdiFlush();
        for (int y = 0; y < d->h; y++) memcpy((uint8_t *)d->bits + (size_t)y * d->pitch, d->dibbits + (size_t)y * d->stride, (size_t)d->w * d->bpp / 8);
        SelectObject(d->dc, d->old); DeleteObject(d->dib); DeleteDC(d->dc);
        memset(d, 0, sizeof *d);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* memory + lazy readback                                               */
void *surf_mem_alloc(size_t n) {
    size_t sz = (n + 4095) & ~(size_t)4095;
    /* write watch: lets the exception-free mode find the rows the game wrote without page faults */
    void *p = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE | 0x200000 /* MEM_WRITE_WATCH */, PAGE_READWRITE);
    return p ? p : VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
/* pages of [base,base+len) written since the last reset; returns -1 if not available */
int plat_write_watch(void *base, size_t len, int reset, void **pages, size_t *n) {
    uintptr_t cnt = *n; DWORD gran = 0;
    if (GetWriteWatch(reset ? 1 : 0, base, len, pages, &cnt, &gran) != 0) return -1;
    *n = cnt; return 0;
}
void surf_mem_free(void *p, size_t n) { (void)n; if (p) VirtualFree(p, 0, MEM_RELEASE); }
int plat_protect(void *p, size_t n, int mode) {
    DWORD old;
    return VirtualProtect(p, n, mode == 0 ? PAGE_NOACCESS : mode == 1 ? PAGE_READONLY : PAGE_READWRITE, &old) ? 0 : -1;
}
typedef struct { void *base, *alloc_base; DWORD alloc_prot; size_t size; DWORD state, prot, type; } MBI_W;
DLLIMP size_t WINAPI VirtualQuery(const void *, MBI_W *, size_t);
DLLIMP void *WINAPI SetUnhandledExceptionFilter(void *);
uintptr_t plat_module_of(void *addr, char *name, int n) {
    MBI_W m; char path[260];
    if (VirtualQuery(addr, &m, sizeof m) && m.alloc_base && GetModuleFileNameA((HMODULE)(uintptr_t)m.alloc_base, path, sizeof path)) {
        const char *b = strrchr(path, '\\'); b = b ? b + 1 : path;
        gp3_snprintf(name, n, "%s", b);
        return (uintptr_t)m.alloc_base;
    }
    gp3_snprintf(name, n, "%08x(no module)", (unsigned)((uintptr_t)addr & ~0xffffu));
    return (uintptr_t)addr & ~0xffffu;
}
static void describe_addr(uintptr_t a, char *o, int n) {
    MBI_W m; char path[260] = "?";
    if (VirtualQuery((void *)a, &m, sizeof m) && m.alloc_base &&
        GetModuleFileNameA((HMODULE)(uintptr_t)m.alloc_base, path, sizeof path)) {
        const char *b = strrchr(path, '\\'); b = b ? b + 1 : path;
        snprintf(o, n, "%s+0x%x", b, (unsigned)(a - (uintptr_t)m.alloc_base));
    } else snprintf(o, n, "%08x (no module)", (unsigned)a);
}
static void log_exception(const char *what, EXCEPTION_POINTERS_W *ep) {
    EXCEPTION_RECORD_W *r = ep->rec;
    const uint8_t *c = ep->ctx;
    uintptr_t eip = c ? *(const DWORD *)(c + 0xB8) : (uintptr_t)r->address;
    uintptr_t esp = c ? *(const DWORD *)(c + 0xC4) : 0;
    char where[300]; describe_addr(eip, where, sizeof where);
    LOG("%s: code %08x at %s, eip %08x esp %08x, info %x %08x", what, (unsigned)r->code, where, (unsigned)eip,
        (unsigned)esp, r->nparams > 0 ? (unsigned)r->info[0] : 0, r->nparams > 1 ? (unsigned)r->info[1] : 0);
    if (esp) {   /* raw stack words that point into modules (rough call stack) */
        int shown = 0;
        for (int i = 0; i < 256 && shown < 12; i++) {
            DWORD v = ((const DWORD *)esp)[i];
            MBI_W m;
            if (v < 0x10000 || !VirtualQuery((void *)(uintptr_t)v, &m, sizeof m) || m.type != 0x1000000 || !(m.prot & 0xF0)) continue;
            describe_addr(v, where, sizeof where); LOG("  stack[%d] %s", i, where); shown++;
        }
    }
}
int plat_make_writable(void *addr, int n) { DWORD old; return VirtualProtect(addr, n, 0x04 /* PAGE_READWRITE */, &old) ? 0 : -1; }
int plat_patch_code(void *addr, const void *bytes, int n) {
    DWORD old;
    if (!VirtualProtect(addr, n, 0x40 /* PAGE_EXECUTE_READWRITE */, &old)) return -1;
    memcpy(addr, bytes, n);
    VirtualProtect(addr, n, old, &old);
    typedef BOOL (WINAPI *fic_t)(HANDLE, const void *, size_t);
    static fic_t fic; if (!fic) fic = (fic_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "FlushInstructionCache");
    if (fic) fic((HANDLE)(uintptr_t)-1, addr, n);
    return 0;
}
static LONG WINAPI veh(EXCEPTION_POINTERS_W *ep) {
    EXCEPTION_RECORD_W *r = ep->rec;
    if (r->code == 0x80000003 && ep->ctx) {      /* shadow texel trap (d3d.c) */
        extern int d3d_is_shadow_site(uint32_t a); extern int d3d_shadow_read(uint32_t addr, uint8_t *out);
        uint8_t *c = ep->ctx;
        DWORD *eip = (DWORD *)(c + 0xB8), *ecx = (DWORD *)(c + 0xAC), edi = *(DWORD *)(c + 0x9C);
        uint32_t site = d3d_is_shadow_site(*eip) ? *eip : d3d_is_shadow_site(*eip - 1) ? *eip - 1 : 0;
        uint8_t v;
        if (site && d3d_shadow_read(edi, &v)) { *ecx = (*ecx & ~0xffu) | v; *eip = site + 2; return -1; }
    }
    if (r->code == 0xC0000005 && r->nparams >= 2) {
        if (d3d_guard_fault((void *)r->info[1], r->info[0] == 1)) return -1;   /* EXCEPTION_CONTINUE_EXECUTION */
    }
    static int logged;
    if ((r->code & 0xF0000000) == 0xC0000000 && logged < 8) { logged++; log_exception("exception", ep); }
    return 0;   /* EXCEPTION_CONTINUE_SEARCH */
}
static LONG WINAPI unhandled(EXCEPTION_POINTERS_W *ep) {
    log_exception("UNHANDLED exception (the game will close)", ep);
    return 0;
}

extern rbackend_t *(*g_platform_backend)(void);
extern int g_lazy_readback;
void win_platform_init(void) {
    typedef BOOL (WINAPI *dpi_t)(void);
    HMODULE u = GetModuleHandleA("user32.dll");
    dpi_t dpi = u ? (dpi_t)GetProcAddress(u, "SetProcessDPIAware") : NULL;
    if (dpi) dpi();                         /* real pixels on high-DPI screens (no blurry OS scaling) */
    timeBeginPeriod(1);
    AddVectoredExceptionHandler(1, veh);
    SetUnhandledExceptionFilter((void *)unhandled);
    g_platform_backend = win_backend_create;
    g_lazy_readback = g_wcfg.lazy_readback;
    { extern int g_nofault; if (g_nofault) { g_lazy_readback = 0; LOG("debugger attached (e.g. GPxPatch): exception-free mode"); } }
}
void video_init(void) {}

/* called when the game exits: let go of the OpenGL context before the process
 * tears down (some NVIDIA/Optimus drivers hang in their DLL detach otherwise) */
void win_gl_shutdown(void) {
    if (!g_gl_rc) return;
    HGLRC rc = g_gl_rc; g_gl_rc = 0;
    wglMakeCurrent(0, 0);
    wglDeleteContext(rc);
    if (g_gl_dc && g_hwnd) ReleaseDC(gl_surface(), g_gl_dc);
    g_gl_dc = 0;
}

int plat_mkdir(const char *path) { return CreateDirectoryA(path, NULL) ? 0 : -1; }

/* ---- "not responding" watchdog ----
 * Records when the game window's thread last ran its message loop. If it stops
 * for more than 2 s (outside GetMessage), the thread is paused for a moment and
 * its location and call stack are written to gp3rt.log. */
DLLIMP HANDLE WINAPI CreateThread(void *, size_t, DWORD (WINAPI *)(void *), void *, DWORD, DWORD *);
DLLIMP HANDLE WINAPI OpenThread(DWORD, BOOL, DWORD);
DLLIMP DWORD WINAPI SuspendThread(HANDLE);
DLLIMP DWORD WINAPI ResumeThread(HANDLE);
DLLIMP BOOL WINAPI GetThreadContext(HANDLE, void *);
DLLIMP BOOL WINAPI CloseHandle(HANDLE);
static volatile DWORD g_pump_tick, g_wnd_tid; static volatile int g_in_getmsg;

static volatile uint32_t g_npump_wnd, g_npump_other;
void win_pump_mark(int phase) {
    if (!g_wnd_tid && g_hwnd) g_wnd_tid = GetWindowThreadProcessId(g_hwnd, NULL);
    if (GetCurrentThreadId() != g_wnd_tid) { g_npump_other++; return; }
    g_npump_wnd++;
    g_pump_tick = GetTickCount();
    g_in_getmsg = phase == 1;
}
static void dump_thread(DWORD tid, const char *why) {
    HANDLE th = OpenThread(0x0002 /*SUSPEND_RESUME*/ | 0x0008 /*GET_CONTEXT*/ | 0x0040 /*QUERY_INFORMATION*/, 0, tid);
    if (!th) { LOG("watchdog: cannot open thread %u", (unsigned)tid); return; }
    static uint8_t ctx[720] __attribute__((aligned(16)));
    memset(ctx, 0, sizeof ctx); *(DWORD *)ctx = 0x10001 /*CONTEXT_CONTROL*/ | 0x10002 /*INTEGER*/;
    if (SuspendThread(th) != (DWORD)-1) {
        if (GetThreadContext(th, ctx)) {
            DWORD eip = *(DWORD *)(ctx + 0xB8), esp = *(DWORD *)(ctx + 0xC4);
            char w[300]; describe_addr(eip, w, sizeof w);
            extern os_mutex_t g_dd_lock;
            LOG("watchdog: %s - thread %u at %s (esp %08x); ddraw lock owner %u, GL context on thread %u",
                why, (unsigned)tid, w, (unsigned)esp, (unsigned)(uintptr_t)((void **)&g_dd_lock)[3], (unsigned)g_ctx_tid);
            int shown = 0;
            for (int i = 0; i < 1024 && shown < 16; i++) {
                DWORD v = ((volatile DWORD *)(uintptr_t)esp)[i];
                MBI_W m;
                if (v < 0x10000 || !VirtualQuery((void *)(uintptr_t)v, &m, sizeof m) || m.type != 0x1000000 || !(m.prot & 0xF0)) continue;
                describe_addr(v, w, sizeof w); LOG("  stack[%d] %s", i, w); shown++;
            }
        }
        ResumeThread(th);
    }
    CloseHandle(th);
}
static DWORD WINAPI watchdog_main(void *arg) {
    (void)arg;
    int episodes = 0, stalled = 0; DWORD last_status = 0;
    extern unsigned g_rdr_tid;
    for (;;) {
        Sleep(250);
        DWORD tid = g_wnd_tid, t = g_pump_tick, now = GetTickCount();
        {   /* a launcher can also restrict the CPU cores after the game started */
            static DWORD last_aff; extern int win_fix_affinity(int quiet);
            if (now - last_aff >= 1000) { last_aff = now; win_fix_affinity(1); }
        }
        if (g_hwnd && GetForegroundWindow() == g_hwnd && (GetAsyncKeyState(0x7A) & 1) && GetAsyncKeyState(0x11) < 0) {
            extern void rdr_framedump_arm(void); rdr_framedump_arm(); g_shot_pending = 1; LOG("frame dump requested (Ctrl+F11, polled)");
        }
        if (!tid && g_hwnd) tid = g_wnd_tid = GetWindowThreadProcessId(g_hwnd, NULL);
        if (!tid) continue;
        int in_proc = g_in_proc > 0;
        DWORD proc_age = in_proc ? now - g_proc_enter_tick : 0, pump_age = t ? now - t : 0;
        int bad = (in_proc && proc_age > 2000) || (!g_in_getmsg && t && pump_age > 2000) || g_ghost_seen;
        if (now - last_status >= 10000) {
            LOG("watchdog: window thread %u, render thread %u, last message loop %u ms ago%s, window procedure %s; message-loop calls/10s: window thread %u, other threads %u",
                (unsigned)tid, g_rdr_tid, (unsigned)pump_age, g_in_getmsg ? " (waiting in GetMessage)" : "",
                in_proc ? "running" : "idle", g_npump_wnd, g_npump_other);
            g_npump_wnd = g_npump_other = 0;
            last_status = now;
        }
        if (bad) {
            if (!stalled && episodes < 6) {
                episodes++;
                char why[160];
                gp3_snprintf(why, sizeof why, "window not responding (%s; window procedure %s for %u ms, message 0x%x; message loop %u ms ago)",
                             g_ghost_seen ? "Windows shows it as hung" : "stalled", in_proc ? "busy" : "idle", (unsigned)proc_age,
                             (unsigned)g_proc_msg, (unsigned)pump_age);
                dump_thread(tid, why);
                if (g_rdr_tid && g_rdr_tid != tid) dump_thread(g_rdr_tid, "render thread at the same moment");
            }
            stalled = 1; g_ghost_seen = 0;
        } else if (stalled) { LOG("watchdog: window responding again"); stalled = 0; }
    }
    return 0;
}
void win_watchdog_start(void) { DWORD id; CreateThread(NULL, 0, watchdog_main, NULL, 0, &id); }
