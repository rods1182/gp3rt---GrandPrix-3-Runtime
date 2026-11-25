/* USER32: windows, message queue, keyboard helpers, message boxes. */
#include "rt.h"
#include <unistd.h>
#include <sys/syscall.h>
#include "plat.h"
#include <pthread.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>

typedef LRESULT (WINAPI *wndproc_t)(HWND, UINT, WPARAM, LPARAM);

typedef struct {
    char name[64]; wndproc_t proc; UINT style; uint32_t inst, icon, cursor, bg;
} wclass_t;
typedef struct {
    int used; HWND h; wclass_t *cls; wndproc_t proc; char title[128];
    DWORD style, exstyle; int x, y, w, h_; int visible; LONG userdata; HWND parent;
    DWORD owner_wtid; /* creating thread's Windows thread id */
    long owner_tid;   /* creating thread: SendMessage from other threads is executed there (as on Windows) */
} window_t;

static wclass_t g_classes[32];
static window_t g_wins[32];
#define HWND_BASE 0x10010u
HWND g_main_hwnd;
HWND g_focus_hwnd;

static window_t *win_get(HWND h) {
    if (h < HWND_BASE) return NULL;
    uint32_t i = (h - HWND_BASE) / 4;
    if (i >= 32 || !g_wins[i].used) return NULL;
    return &g_wins[i];
}

/* ------------------------------------------------------------------ */
/* message queue                                                        */
typedef struct { HWND hwnd; UINT msg; WPARAM w; LPARAM l; DWORD time; LONG x, y; } msg_t;
#define QSIZE 1024
static msg_t g_q[QSIZE];
static int g_qh, g_qt;
static pthread_mutex_t g_qmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_qcv = PTHREAD_COND_INITIALIZER;
static int g_quit_posted; static int g_quit_code;
static LONG g_mouse_x, g_mouse_y;

void win_post_message(HWND h, UINT msg, WPARAM w, LPARAM l) {
    pthread_mutex_lock(&g_qmx);
    int n = (g_qt + 1) % QSIZE;
    if (n != g_qh) {
        g_q[g_qt] = (msg_t){ h, msg, w, l, rt_ticks_ms(), g_mouse_x, g_mouse_y };
        g_qt = n;
        pthread_cond_broadcast(&g_qcv);
    }
    pthread_mutex_unlock(&g_qmx);
}

static int match(msg_t *m, HWND h, UINT lo, UINT hi) {
    if (h && m->hwnd != h) return 0;
    if (lo || hi) { if (m->msg < lo || m->msg > hi) return 0; }
    return 1;
}
static int q_take(msg_t *out, HWND h, UINT lo, UINT hi, int remove) {
    pthread_mutex_lock(&g_qmx);
    for (int i = g_qh; i != g_qt; i = (i + 1) % QSIZE) {
        if (!match(&g_q[i], h, lo, hi)) continue;
        *out = g_q[i];
        if (remove) {
            for (int j = i; j != g_qh;) { int p = (j - 1 + QSIZE) % QSIZE; g_q[j] = g_q[p]; j = p; }
            g_qh = (g_qh + 1) % QSIZE;
        }
        pthread_mutex_unlock(&g_qmx);
        return 1;
    }
    pthread_mutex_unlock(&g_qmx);
    return 0;
}

/* keyboard state for GetAsyncKeyState / GetKeyState */
uint8_t g_keystate[256];

void win_input_key(int vk, int scancode, int down) {
    if (vk <= 0 || vk > 255) return;
    { extern int replayctl_key(int vk, int down); if (replayctl_key(vk, down)) return; }
    int was = g_keystate[vk] & 0x80;
    g_keystate[vk] = down ? 0x80 : 0;
    LPARAM l = 1 | ((scancode & 0x1ff) << 16);
    if (scancode & 0x100) l |= 1 << 24;
    if (!down) l |= (1u << 30) | (1u << 31);
    else if (was) l |= 1 << 30;
    HWND h = g_focus_hwnd ? g_focus_hwnd : g_main_hwnd;
    win_post_message(h, down ? 0x100 : 0x101, vk, l);
}
void win_input_char(int ch) { win_post_message(g_focus_hwnd ? g_focus_hwnd : g_main_hwnd, 0x102, ch & 0xff, 1); }
void win_input_mouse(int x, int y, int button, int down) {
    g_mouse_x = x; g_mouse_y = y;
    LPARAM l = (y << 16) | (x & 0xffff);
    UINT msg = 0x200;
    if (button == 1) msg = down ? 0x201 : 0x202;
    else if (button == 2) msg = down ? 0x204 : 0x205;
    win_post_message(g_main_hwnd, msg, 0, l);
}

static void write_msg(uint32_t *gm, msg_t *m) {
    gm[0] = m->hwnd; gm[1] = m->msg; gm[2] = m->w; gm[3] = (uint32_t)m->l; gm[4] = m->time; gm[5] = m->x; gm[6] = m->y;
}

/* cross-thread SendMessage: the sender waits until the window's thread runs the
 * window procedure inside its next GetMessage/PeekMessage call. */
static struct { int pending, done; HWND h; UINT msg; WPARAM w; LPARAM l; LRESULT r; long tid; } g_xs;
static pthread_mutex_t g_xmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_xcv = PTHREAD_COND_INITIALIZER;
int g_xthread_send = 1;
static void run_sent_messages(void) {
    long me = syscall(SYS_gettid);
    pthread_mutex_lock(&g_xmx);
    while (g_xs.pending && !g_xs.done && g_xs.tid == me) {
        HWND h = g_xs.h; UINT msg = g_xs.msg; WPARAM w = g_xs.w; LPARAM l = g_xs.l;
        pthread_mutex_unlock(&g_xmx);
        window_t *wn = win_get(h);
        LRESULT r = wn && wn->proc ? wn->proc(h, msg, w, l) : 0;
        pthread_mutex_lock(&g_xmx);
        g_xs.r = r; g_xs.done = 1;
        pthread_cond_broadcast(&g_xcv);
    }
    pthread_mutex_unlock(&g_xmx);
}
static LRESULT send_cross_thread(window_t *wn, HWND h, UINT msg, WPARAM w, LPARAM l) {
    pthread_mutex_lock(&g_xmx);
    while (g_xs.pending) pthread_cond_wait(&g_xcv, &g_xmx);
    g_xs.pending = 1; g_xs.done = 0; g_xs.h = h; g_xs.msg = msg; g_xs.w = w; g_xs.l = l; g_xs.tid = wn->owner_tid;
    pthread_cond_broadcast(&g_qcv);          /* wake the window thread's GetMessage */
    while (!g_xs.done) pthread_cond_wait(&g_xcv, &g_xmx);
    LRESULT r = g_xs.r; g_xs.pending = 0;
    pthread_cond_broadcast(&g_xcv);
    pthread_mutex_unlock(&g_xmx);
    return r;
}

extern void ddraw_flush_present(void);
static BOOL WINAPI PeekMessageA_(uint32_t *gm, HWND h, UINT lo, UINT hi, UINT rm) {
    plat_pump();
    run_sent_messages();
    ddraw_flush_present();
    msg_t m;
    if (q_take(&m, h, lo, hi, rm & 1)) { write_msg(gm, &m); return TRUE; }
    if (g_quit_posted) { m = (msg_t){0, 0x12, g_quit_code, 0, rt_ticks_ms(), 0, 0}; write_msg(gm, &m); if (rm & 1) g_quit_posted = 0; return TRUE; }
    return FALSE;
}
static BOOL WINAPI GetMessageA_(uint32_t *gm, HWND h, UINT lo, UINT hi) {
    for (;;) {
        plat_pump();
        run_sent_messages();
        ddraw_flush_present();
        msg_t m;
        if (q_take(&m, h, lo, hi, 1)) { write_msg(gm, &m); return m.msg != 0x12; }
        if (g_quit_posted) { m = (msg_t){0, 0x12, g_quit_code, 0, rt_ticks_ms(), 0, 0}; write_msg(gm, &m); g_quit_posted = 0; return FALSE; }
        pthread_mutex_lock(&g_qmx);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        if (g_qh == g_qt && !g_xs.pending) pthread_cond_timedwait(&g_qcv, &g_qmx, &ts);
        pthread_mutex_unlock(&g_qmx);
    }
}
int g_text_from_host;   /* Linux bridge: printable characters arrive as text events from the host (keyboard layout aware) */
static BOOL WINAPI TranslateMessage_(const uint32_t *gm) {
    if (gm[1] != 0x100) return FALSE;
    int vk = gm[2]; int ch = 0;
    if (g_text_from_host && vk != 0x0d && vk != 0x08 && vk != 0x1b && vk != 0x09) return FALSE;
    int shift = g_keystate[0x10] & 0x80;
    if (vk >= 'A' && vk <= 'Z') ch = shift ? vk : vk + 32;
    else if (vk >= '0' && vk <= '9') ch = shift ? ")!@#$%^&*("[vk - '0'] : vk;
    else if (vk == 0x20) ch = ' ';
    else if (vk == 0x0d) ch = '\r';
    else if (vk == 0x08) ch = 8;
    else if (vk == 0x1b) ch = 27;
    else if (vk == 0x09) ch = 9;
    else if (vk >= 0x60 && vk <= 0x69) ch = '0' + vk - 0x60;
    else if (vk == 0xbd) ch = shift ? '_' : '-';
    else if (vk == 0xbb) ch = shift ? '+' : '=';
    else if (vk == 0xbe) ch = shift ? '>' : '.';
    else if (vk == 0xbc) ch = shift ? '<' : ',';
    else if (vk == 0xbf) ch = shift ? '?' : '/';
    if (ch) win_post_message(gm[0], 0x102, ch, (LPARAM)gm[3]);
    return ch != 0;
}
LRESULT win_send(HWND h, UINT msg, WPARAM w, LPARAM l) {
    window_t *wn = win_get(h);
    if (!wn || !wn->proc) return 0;
    return wn->proc(h, msg, w, l);
}
static LRESULT WINAPI DispatchMessageA_(const uint32_t *gm) {
    return win_send(gm[0], gm[1], gm[2], (LPARAM)gm[3]);
}
static LRESULT WINAPI SendMessageA_(HWND h, UINT msg, WPARAM w, LPARAM l) {
    window_t *wn = win_get(h);
    if (g_xthread_send && wn && wn->owner_tid && wn->owner_tid != syscall(SYS_gettid)) return send_cross_thread(wn, h, msg, w, l);
    return win_send(h, msg, w, l);
}
static BOOL WINAPI PostMessageA_(HWND h, UINT msg, WPARAM w, LPARAM l) { win_post_message(h, msg, w, l); return TRUE; }
static void WINAPI PostQuitMessage_(int code) { LOG("PostQuitMessage(%d)", code); g_quit_posted = 1; g_quit_code = code; pthread_cond_broadcast(&g_qcv); }
static BOOL WINAPI ReplyMessage_(LRESULT r) { (void)r; return FALSE; }

static LRESULT WINAPI DefWindowProcA_(HWND h, UINT msg, WPARAM w, LPARAM l) {
    (void)w; (void)l;
    switch (msg) {
    case 0x81: return 1;            /* WM_NCCREATE */
    case 0x10: win_send(h, 2, 0, 0); return 0;  /* WM_CLOSE -> WM_DESTROY */
    case 0x14: return 1;            /* WM_ERASEBKGND */
    case 0x20: return 0;            /* WM_SETCURSOR */
    case 0x84: return 1;            /* WM_NCHITTEST -> HTCLIENT */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* windows                                                              */
static WORD WINAPI RegisterClassExA_(const uint32_t *wc) {
    for (int i = 0; i < 32; i++) if (!g_classes[i].name[0]) {
        g_classes[i].style = wc[1]; g_classes[i].proc = (wndproc_t)(uintptr_t)wc[2];
        g_classes[i].inst = wc[5]; g_classes[i].icon = wc[6]; g_classes[i].cursor = wc[7]; g_classes[i].bg = wc[8];
        const char *n = (const char *)(uintptr_t)wc[10];
        if ((uintptr_t)n < 0x10000) snprintf(g_classes[i].name, 64, "#%u", (unsigned)(uintptr_t)n);
        else snprintf(g_classes[i].name, 64, "%s", n);
        LOG("RegisterClass(%s, proc=%p)", g_classes[i].name, (void *)g_classes[i].proc);
        return (WORD)(0xc000 + i);
    }
    return 0;
}
static WORD WINAPI RegisterClassA_(const uint32_t *wc) {
    uint32_t ex[12] = { 48, wc[0], wc[1], wc[2], wc[3], wc[4], wc[5], wc[6], wc[7], wc[8], wc[9], 0 };
    return RegisterClassExA_(ex);
}
static BOOL WINAPI UnregisterClassA_(const char *n, uint32_t inst) { (void)n; (void)inst; return TRUE; }
static wclass_t *class_find(const char *n) {
    for (int i = 0; i < 32; i++) {
        if (!g_classes[i].name[0]) continue;
        if ((uintptr_t)n < 0x10000) { if ((uintptr_t)n == 0xc000u + i) return &g_classes[i]; }
        else if (str_ieq(g_classes[i].name, n)) return &g_classes[i];
    }
    return NULL;
}

static HWND WINAPI CreateWindowExA_(DWORD ex, const char *cls, const char *title, DWORD style, int x, int y, int w, int h,
                                    HWND parent, uint32_t menu, uint32_t inst, void *param) {
    (void)menu; (void)inst;
    wclass_t *c = class_find(cls);
    int i; for (i = 0; i < 32 && g_wins[i].used; i++) {}
    if (i == 32) return 0;
    window_t *wn = &g_wins[i];
    memset(wn, 0, sizeof *wn);
    wn->owner_tid = syscall(SYS_gettid); wn->owner_wtid = teb_current() ? teb_current()->tid : 1;
    wn->used = 1; wn->h = HWND_BASE + i * 4; wn->cls = c; wn->proc = c ? c->proc : DefWindowProcA_;
    snprintf(wn->title, sizeof wn->title, "%s", title ? title : "");
    wn->style = style; wn->exstyle = ex; wn->x = x; wn->y = y; wn->w = w; wn->h_ = h; wn->parent = parent;
    if (x == (int)0x80000000) { wn->x = 0; wn->y = 0; }
    if (w == (int)0x80000000) { wn->w = 640; wn->h_ = 480; }
    LOG("CreateWindowEx(%s, \"%s\", style=%x, %d,%d %dx%d) -> %x",
        (uintptr_t)cls < 0x10000 ? "#atom" : cls, wn->title, style, x, y, w, h, wn->h);
    if (!g_main_hwnd && !parent) g_main_hwnd = wn->h;
    /* CREATESTRUCT */
    uint32_t cs[12] = { (uint32_t)(uintptr_t)param, inst, menu, parent, (uint32_t)h, (uint32_t)w, (uint32_t)y, (uint32_t)x, style,
                        (uint32_t)(uintptr_t)title, (uint32_t)(uintptr_t)cls, ex };
    win_send(wn->h, 0x81, 0, (LPARAM)(uintptr_t)cs);
    if (win_send(wn->h, 0x01, 0, (LPARAM)(uintptr_t)cs) == -1) { wn->used = 0; return 0; }
    if (style & 0x10000000) { wn->visible = 1; }
    return wn->h;
}
static BOOL WINAPI DestroyWindow_(HWND h) {
    window_t *w = win_get(h); if (!w) return FALSE;
    win_send(h, 2, 0, 0);
    w->used = 0;
    return TRUE;
}
static BOOL WINAPI ShowWindow_(HWND h, int cmd) {
    window_t *w = win_get(h); if (!w) return FALSE;
    int was = w->visible;
    w->visible = cmd != 0;
    LOG("ShowWindow(%x, %d)", h, cmd);
    if (!was && w->visible) {
        win_send(h, 0x18, 1, 0);                    /* WM_SHOWWINDOW */
        if (h == g_main_hwnd) {
            win_send(h, 0x1c, 1, 0);                /* WM_ACTIVATEAPP */
            win_send(h, 0x06, 1, 0);                /* WM_ACTIVATE (WA_ACTIVE) */
            win_send(h, 0x07, 0, 0);                /* WM_SETFOCUS */
        }
    }
    return was;
}
static BOOL WINAPI UpdateWindow_(HWND h) { (void)h; return TRUE; }
static HWND WINAPI SetFocus_(HWND h) { HWND o = g_focus_hwnd; g_focus_hwnd = h; return o; }
static BOOL WINAPI SetForegroundWindow_(HWND h) { (void)h; return TRUE; }
static BOOL WINAPI BringWindowToTop_(HWND h) { (void)h; return TRUE; }
static HWND WINAPI GetActiveWindow_(void) { return g_main_hwnd; }
static HWND WINAPI GetForegroundWindow_(void) { return g_main_hwnd; }
static HWND WINAPI FindWindowA_(const char *cls, const char *title) { (void)cls; (void)title; return 0; }
static BOOL WINAPI AdjustWindowRectEx_(LONG *r, DWORD style, BOOL menu, DWORD ex) {
    (void)menu; (void)ex;
    if (style & 0x00C00000) { r[0] -= 3; r[1] -= 26; r[2] += 3; r[3] += 3; }   /* caption + border */
    return TRUE;
}
static BOOL WINAPI GetClientRect_(HWND h, LONG *r) {
    window_t *w = win_get(h);
    r[0] = r[1] = 0;
    int cw, ch; plat_client_size(&cw, &ch);
    r[2] = w && w->w > 0 ? cw : 640; r[3] = w && w->h_ > 0 ? ch : 480;
    return TRUE;
}
static BOOL WINAPI GetWindowRect_(HWND h, LONG *r) {
    window_t *w = win_get(h);
    if (!w) { r[0] = r[1] = 0; plat_client_size((int *)&r[2], (int *)&r[3]); return TRUE; }
    r[0] = w->x; r[1] = w->y; r[2] = w->x + w->w; r[3] = w->y + w->h_;
    return TRUE;
}
static BOOL WINAPI ClientToScreen_(HWND h, LONG *p) { (void)h; (void)p; return TRUE; }
static BOOL WINAPI ScreenToClient_(HWND h, LONG *p) { (void)h; (void)p; return TRUE; }
static BOOL WINAPI SetWindowPos_(HWND h, HWND after, int x, int y, int cx, int cy, UINT fl) {
    (void)after;
    window_t *w = win_get(h); if (!w) return FALSE;
    if (!(fl & 2)) { w->x = x; w->y = y; }
    if (!(fl & 1)) { w->w = cx; w->h_ = cy; }
    return TRUE;
}
static LONG WINAPI SetWindowLongA_(HWND h, int idx, LONG v) {
    window_t *w = win_get(h); if (!w) return 0;
    LONG o = 0;
    switch (idx) {
    case -4: o = (LONG)(uintptr_t)w->proc; w->proc = (wndproc_t)(uintptr_t)v; break;
    case -16: o = w->style; w->style = v; break;
    case -20: o = w->exstyle; w->exstyle = v; break;
    case -21: o = w->userdata; w->userdata = v; break;
    }
    return o;
}
static LONG WINAPI GetWindowLongA_(HWND h, int idx) {
    window_t *w = win_get(h); if (!w) return 0;
    switch (idx) {
    case -4: return (LONG)(uintptr_t)w->proc;
    case -16: return w->style;
    case -20: return w->exstyle;
    case -21: return w->userdata;
    }
    return 0;
}
static DWORD WINAPI GetClassLongA_(HWND h, int idx) { (void)h; (void)idx; return 0; }
static BOOL WINAPI IsWindowVisible_(HWND h) { window_t *w = win_get(h); return w && w->visible; }
static HWND WINAPI GetWindow_(HWND h, UINT c) { (void)h; (void)c; return 0; }
static HWND WINAPI GetTopWindow_(HWND h) { (void)h; return g_main_hwnd; }
static DWORD WINAPI GetWindowThreadProcessId_(HWND h, DWORD *pid) {
    if (pid) *pid = teb_current()->pid;
    unsigned i = ((unsigned)(uintptr_t)h - HWND_BASE) / 4;
    return (h && i < 32 && g_wins[i].used && g_wins[i].owner_wtid) ? g_wins[i].owner_wtid : 1;
}
static int WINAPI GetClassNameA_(HWND h, char *buf, int n) { (void)h; if (n > 0) { snprintf(buf, (size_t)n, "GP3"); return 3; } return 0; }
static BOOL WINAPI AttachThreadInput_(DWORD a, DWORD b, BOOL f) { (void)a; (void)b; (void)f; return TRUE; }
static BOOL WINAPI EnumDisplayDevicesA_(const char *dev, DWORD i, uint8_t *dd, DWORD fl) {   /* one adapter */
    (void)dev; (void)fl; if (i > 0 || !dd) return FALSE;
    memset(dd + 4, 0, 32 + 128); strcpy((char *)dd + 4, "\\\\.\\DISPLAY1"); strcpy((char *)dd + 36, "GP3RT Virtual Display Adapter");
    return TRUE;
}
static int WINAPI GetSystemMetrics_(int i) {
    int w, h; plat_client_size(&w, &h);
    switch (i) { case 0: return w; case 1: return h; case 4: return 26; case 5: case 6: return 1; case 32: case 33: return 3; }
    return 0;
}
static uint32_t WINAPI LoadIconA_(uint32_t inst, const char *n) { (void)inst; (void)n; return 0x20001; }
static uint32_t WINAPI LoadCursorA_(uint32_t inst, const char *n) { (void)inst; (void)n; return 0x20002; }
static uint32_t WINAPI SetCursor_(uint32_t c) { (void)c; return 0x20002; }
static int g_cursor_count;
static int WINAPI ShowCursor_(BOOL s) { g_cursor_count += s ? 1 : -1; plat_show_cursor(g_cursor_count >= 0); return g_cursor_count; }
static BOOL WINAPI GetCursorPos_(LONG *p) { p[0] = g_mouse_x; p[1] = g_mouse_y; return TRUE; }
static uint32_t WINAPI BeginPaint_(HWND h, uint8_t *ps) { (void)h; memset(ps, 0, 64); *(uint32_t *)ps = 0x30001; return 0x30001; }
static BOOL WINAPI EndPaint_(HWND h, void *ps) { (void)h; (void)ps; return TRUE; }
static DWORD WINAPI GetSysColor_(int i) { (void)i; return 0; }
static BOOL WINAPI SetSysColors_(int n, const int *e, const DWORD *v) { (void)n; (void)e; (void)v; return TRUE; }
static LONG WINAPI ChangeDisplaySettingsA_(void *dm, DWORD fl) { (void)dm; (void)fl; return 0; }

/* ------------------------------------------------------------------ */
/* keys                                                                 */
static const char *vk_names[256] = {
    [0x08] = "Backspace", [0x09] = "Tab", [0x0d] = "Enter", [0x10] = "Shift", [0x11] = "Ctrl", [0x12] = "Alt",
    [0x13] = "Pause", [0x14] = "Caps Lock", [0x1b] = "Esc", [0x20] = "Space", [0x21] = "Page Up", [0x22] = "Page Down",
    [0x23] = "End", [0x24] = "Home", [0x25] = "Left", [0x26] = "Up", [0x27] = "Right", [0x28] = "Down",
    [0x2d] = "Insert", [0x2e] = "Delete", [0x60] = "Num 0", [0x61] = "Num 1", [0x62] = "Num 2", [0x63] = "Num 3",
    [0x64] = "Num 4", [0x65] = "Num 5", [0x66] = "Num 6", [0x67] = "Num 7", [0x68] = "Num 8", [0x69] = "Num 9",
    [0x6a] = "Num *", [0x6b] = "Num +", [0x6d] = "Num -", [0x6e] = "Num Del", [0x6f] = "Num /",
    [0x70] = "F1", [0x71] = "F2", [0x72] = "F3", [0x73] = "F4", [0x74] = "F5", [0x75] = "F6", [0x76] = "F7",
    [0x77] = "F8", [0x78] = "F9", [0x79] = "F10", [0x7a] = "F11", [0x7b] = "F12", [0x90] = "Num Lock",
    [0x91] = "Scroll Lock", [0xba] = ";", [0xbb] = "=", [0xbc] = ",", [0xbd] = "-", [0xbe] = ".", [0xbf] = "/",
    [0xc0] = "`", [0xdb] = "[", [0xdc] = "\\", [0xdd] = "]", [0xde] = "'",
};
/* scan code (set 1) <-> virtual key */
static const uint8_t sc2vk[128] = {
    0, 0x1b, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xbd, 0xbb, 0x08, 0x09,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xdb, 0xdd, 0x0d, 0x11, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xba, 0xde, 0xc0, 0x10, 0xdc, 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', 0xbc, 0xbe, 0xbf, 0x10, 0x6a, 0x12, 0x20, 0x14, 0x70, 0x71, 0x72, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x90, 0x91, 0x67, 0x68, 0x69, 0x6d, 0x64, 0x65, 0x66, 0x6b, 0x61,
    0x62, 0x63, 0x60, 0x6e, 0, 0, 0, 0x7a, 0x7b,
};
int vk_to_scancode(int vk) {
    switch (vk) { case 0x25: return 0x14b; case 0x26: return 0x148; case 0x27: return 0x14d; case 0x28: return 0x150;
                  case 0x2d: return 0x152; case 0x2e: return 0x153; case 0x24: return 0x147; case 0x23: return 0x14f;
                  case 0x21: return 0x149; case 0x22: return 0x151; case 0x6f: return 0x135; }
    for (int i = 1; i < 128; i++) if (sc2vk[i] == vk) return i;
    return 0;
}
static UINT WINAPI MapVirtualKeyA_(UINT code, UINT type) {
    if (type == 0) return vk_to_scancode(code) & 0xff;
    if (type == 1 || type == 3) { if (code < 128) return sc2vk[code]; return 0; }
    if (type == 2) { if (code >= 'A' && code <= 'Z') return code; if (code >= '0' && code <= '9') return code; return 0; }
    return 0;
}
static int WINAPI GetKeyNameTextA_(LONG l, char *buf, int n) {
    int sc = (l >> 16) & 0x1ff;
    int vk = 0;
    if (sc & 0x100) {
        switch (sc & 0xff) { case 0x4b: vk = 0x25; break; case 0x48: vk = 0x26; break; case 0x4d: vk = 0x27; break; case 0x50: vk = 0x28; break;
                             case 0x52: vk = 0x2d; break; case 0x53: vk = 0x2e; break; case 0x47: vk = 0x24; break; case 0x4f: vk = 0x23; break;
                             case 0x49: vk = 0x21; break; case 0x51: vk = 0x22; break; case 0x1c: vk = 0x0d; break; case 0x1d: vk = 0x11; break;
                             case 0x38: vk = 0x12; break; case 0x35: vk = 0x6f; break; }
    } else if (sc < 128) vk = sc2vk[sc];
    char tmp[32];
    if (vk >= 'A' && vk <= 'Z') snprintf(tmp, sizeof tmp, "%c", vk);
    else if (vk >= '0' && vk <= '9') snprintf(tmp, sizeof tmp, "%c", vk);
    else if (vk && vk_names[vk]) snprintf(tmp, sizeof tmp, "%s", vk_names[vk]);
    else tmp[0] = 0;
    snprintf(buf, n, "%s", tmp);
    return (int)strlen(buf);
}
static short WINAPI GetAsyncKeyState_(int vk) { return (vk >= 0 && vk < 256 && (g_keystate[vk] & 0x80)) ? (short)0x8000 : 0; }
static short WINAPI GetKeyState_(int vk) { return GetAsyncKeyState_(vk); }
static char *WINAPI CharLowerA_(char *s) {
    if ((uintptr_t)s < 0x10000) { unsigned c = (unsigned)(uintptr_t)s; return (char *)(uintptr_t)((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7) ? c + 32 : c); }
    for (unsigned char *p = (unsigned char *)s; *p; p++) if ((*p >= 'A' && *p <= 'Z') || (*p >= 0xc0 && *p <= 0xde && *p != 0xd7)) *p += 32;
    return s;
}
static char *WINAPI CharUpperA_(char *s) {
    if ((uintptr_t)s < 0x10000) { unsigned c = (unsigned)(uintptr_t)s; return (char *)(uintptr_t)((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7) ? c - 32 : c); }
    for (unsigned char *p = (unsigned char *)s; *p; p++) if ((*p >= 'a' && *p <= 'z') || (*p >= 0xe0 && *p <= 0xfe && *p != 0xf7)) *p -= 32;
    return s;
}

/* ------------------------------------------------------------------ */
/* wvsprintfA / wsprintfA (Windows semantics, 32-bit args)              */
int win_vsprintf(char *out, const char *fmt, const uint32_t *args) {
    char *o = out;
    while (*fmt) {
        if (*fmt != '%') { *o++ = *fmt++; continue; }
        fmt++;
        if (*fmt == '%') { *o++ = '%'; fmt++; continue; }
        char spec[32]; int sl = 0; spec[sl++] = '%';
        while (*fmt && strchr("-+ #0", *fmt) && sl < 20) spec[sl++] = *fmt++;
        while (*fmt && isdigit((unsigned char)*fmt) && sl < 24) spec[sl++] = *fmt++;
        if (*fmt == '.') { spec[sl++] = *fmt++; while (*fmt && isdigit((unsigned char)*fmt) && sl < 28) spec[sl++] = *fmt++; }
        int wide = 0;
        if (*fmt == 'l') { fmt++; } else if (*fmt == 'h') { fmt++; } else if (*fmt == 'L') { fmt++; }
        if (*fmt == 'I' && fmt[1] == '6' && fmt[2] == '4') { fmt += 3; }
        char c = *fmt++;
        if (c == 'S') wide = 1;
        switch (c) {
        case 'd': case 'i': spec[sl++] = 'd'; spec[sl] = 0; o += sprintf(o, spec, (int32_t)*args++); break;
        case 'u': spec[sl++] = 'u'; spec[sl] = 0; o += sprintf(o, spec, (uint32_t)*args++); break;
        case 'x': case 'X': case 'o': spec[sl++] = c; spec[sl] = 0; o += sprintf(o, spec, (uint32_t)*args++); break;
        case 'p': o += sprintf(o, "%08X", (uint32_t)*args++); break;
        case 'c': case 'C': spec[sl++] = 'c'; spec[sl] = 0; o += sprintf(o, spec, (int)(*args++ & 0xff)); break;
        case 's': case 'S': {
            const char *s = (const char *)(uintptr_t)*args++;
            char tmp[1024];
            if (wide && s) { const WCHAR *w = (const WCHAR *)s; int i = 0; for (; w[i] && i < 1023; i++) tmp[i] = (char)w[i]; tmp[i] = 0; s = tmp; }
            spec[sl++] = 's'; spec[sl] = 0; o += sprintf(o, spec, s ? s : "(null)"); break;
        }
        default: *o++ = '%'; *o++ = c; break;
        }
    }
    *o = 0;
    return (int)(o - out);
}
static int WINAPI wvsprintfA_(char *out, const char *fmt, const uint32_t *args) { return win_vsprintf(out, fmt, args); }
static int CDECL wsprintfA_(char *out, const char *fmt, ...) {
    /* cdecl varargs: arguments follow fmt on the stack */
    const uint32_t *args = (const uint32_t *)(&fmt + 1);
    return win_vsprintf(out, fmt, args);
}

static int WINAPI MessageBoxA_(HWND h, const char *text, const char *cap, UINT type) {
    (void)h;
    WARN("MessageBox [%s]: %s (type %x)", cap ? cap : "", text ? text : "", type);
    plat_message_box(cap, text);
    UINT b = type & 0xf;
    return b == 4 || b == 3 ? 6 /* IDYES */ : 1 /* IDOK */;
}
static int WINAPI MessageBoxExA_(HWND h, const char *text, const char *cap, UINT type, WORD lang) { (void)lang; return MessageBoxA_(h, text, cap, type); }

/* ------------------------------------------------------------------ */
/* DCs for windows are handled by gdi32.c */
extern uint32_t gdi_window_dc(HWND h);
static uint32_t WINAPI GetDC_(HWND h) { return gdi_window_dc(h); }
static int WINAPI ReleaseDC_(HWND h, uint32_t dc) { (void)h; (void)dc; return 1; }
extern uint32_t gdi_load_image(const char *file, UINT type, int cx, int cy, UINT fl);
static uint32_t WINAPI LoadImageA_(uint32_t inst, const char *name, UINT type, int cx, int cy, UINT fl) {
    if (!(fl & 0x10) || (uintptr_t)name < 0x10000) {
        static int n; if (n++ < 30) LOG("LoadImageA(inst %x, %s, type %u, %dx%d, flags %x) unsupported", inst, (uintptr_t)name < 0x10000 ? "#id" : name, type, cx, cy, fl);
        return type == 1 || type == 2 ? 0x20001 : 0;
    }
    (void)inst;
    return gdi_load_image(name, type, cx, cy, fl);
}

static LRESULT WINAPI CallWindowProcA_(uint32_t proc, HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!proc) return 0;
    return ((wndproc_t)(uintptr_t)proc)(h, m, w, l);
}
static uint32_t WINAPI MonitorFromWindow_(HWND h, DWORD f) { (void)h; (void)f; return 0x7f0001; }
static BOOL WINAPI GetMonitorInfoA_(uint32_t mon, DWORD *mi) {
    (void)mon;
    int w = 1920, h = 1080;
    mi[1] = 0; mi[2] = 0; mi[3] = w; mi[4] = h;        /* rcMonitor */
    mi[5] = 0; mi[6] = 0; mi[7] = w; mi[8] = h - 40;   /* rcWork */
    mi[9] = 1;
    return TRUE;
}
static BOOL WINAPI SetProcessDPIAware_(void) { return TRUE; }
#define E(n) { "user32", #n, 0, (void *)n##_ }
static const export_t user32_exports[] = {
    E(PeekMessageA), E(GetMessageA), E(TranslateMessage), E(DispatchMessageA), E(SendMessageA), E(PostMessageA),
    E(PostQuitMessage), E(ReplyMessage), E(DefWindowProcA), E(RegisterClassExA), E(RegisterClassA), E(UnregisterClassA),
    E(CreateWindowExA), E(DestroyWindow), E(ShowWindow), E(UpdateWindow), E(SetFocus), E(SetForegroundWindow), E(BringWindowToTop),
    E(GetActiveWindow), E(GetForegroundWindow), E(FindWindowA), E(AdjustWindowRectEx), E(GetClientRect), E(GetWindowRect),
    E(ClientToScreen), E(ScreenToClient), E(SetWindowPos), E(SetWindowLongA), E(GetWindowLongA), E(GetClassLongA),
    E(IsWindowVisible), E(GetWindow), E(GetTopWindow), E(GetWindowThreadProcessId), E(GetSystemMetrics), E(EnumDisplayDevicesA), E(GetClassNameA), E(AttachThreadInput), E(LoadIconA),
    E(LoadCursorA), E(SetCursor), E(ShowCursor), E(GetCursorPos), E(BeginPaint), E(EndPaint), E(GetSysColor),
    E(SetSysColors), E(ChangeDisplaySettingsA), E(MapVirtualKeyA), E(GetKeyNameTextA), E(GetAsyncKeyState),
    E(GetKeyState), E(CharLowerA), E(CharUpperA), E(wvsprintfA), E(wsprintfA), E(MessageBoxA), E(MessageBoxExA),
    E(GetDC), E(ReleaseDC), E(LoadImageA), E(CallWindowProcA), E(MonitorFromWindow), E(GetMonitorInfoA), E(SetProcessDPIAware),
    { NULL, NULL, 0, NULL }
};
#undef E
void user32_register(void) { rt_register_exports(user32_exports); }

void replayctl_inject_key(int vk, int scancode, int down) { win_input_key(vk, scancode, down); }
