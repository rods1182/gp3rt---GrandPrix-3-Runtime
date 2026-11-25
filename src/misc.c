/* WINMM (timers, joystick), ADVAPI32, OLE32, WINSPOOL, WSOCK32 and the
 * built-in binkw32 fallback, plus registration of every export table. */
#include "rt.h"
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* WINMM                                                                */
typedef void (WINAPI *timecb_t)(UINT id, UINT msg, DWORD user, DWORD d1, DWORD d2);
typedef struct { int used; UINT id; UINT delay; timecb_t cb; DWORD user; UINT flags; volatile int stop; pthread_t th; } mmtimer_t;
static mmtimer_t g_timers[16];
static pthread_mutex_t g_tmx = PTHREAD_MUTEX_INITIALIZER;
extern int rt_thread_create(DWORD (WINAPI *fn)(void *), void *arg, DWORD *tid, pthread_t *out);

static DWORD WINAPI timer_thread(void *p) {
    mmtimer_t *t = p;
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t period_ns = (uint64_t)t->delay * 1000000ull;
    for (;;) {
        next.tv_nsec += period_ns % 1000000000ull; next.tv_sec += period_ns / 1000000000ull;
        if (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {}
        if (t->stop) break;
        t->cb(t->id, 0, t->user, 0, 0);
        if (!(t->flags & 1)) break;    /* TIME_ONESHOT */
        /* if we fell far behind (debugger, heavy load), resynchronise instead of bursting */
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t lag = (int64_t)(now.tv_sec - next.tv_sec) * 1000000000LL + (now.tv_nsec - next.tv_nsec);
        if (lag > (int64_t)period_ns * 8) next = now;
    }
    t->used = 0;
    return 0;
}
static UINT WINAPI timeSetEvent_(UINT delay, UINT res, timecb_t cb, DWORD user, UINT flags) {
    (void)res;
    pthread_mutex_lock(&g_tmx);
    int i; for (i = 0; i < 16 && g_timers[i].used; i++) {}
    if (i == 16) { pthread_mutex_unlock(&g_tmx); return 0; }
    mmtimer_t *t = &g_timers[i];
    static UINT next_id = 1;
    t->used = 1; t->id = next_id++; t->delay = delay ? delay : 1; t->cb = cb; t->user = user; t->flags = flags; t->stop = 0;
    pthread_mutex_unlock(&g_tmx);
    LOG("timeSetEvent(delay=%u, cb=%p, flags=%x) -> %u", delay, (void *)cb, flags, t->id);
    rt_thread_create(timer_thread, t, NULL, &t->th);
    return t->id;
}
static UINT WINAPI timeKillEvent_(UINT id) {
    for (int i = 0; i < 16; i++) if (g_timers[i].used && g_timers[i].id == id) { g_timers[i].stop = 1; return 0; }
    return 97;
}
static UINT WINAPI timeBeginPeriod_(UINT p) { (void)p; return 0; }
static UINT WINAPI timeEndPeriod_(UINT p) { (void)p; return 0; }
static UINT WINAPI timeGetDevCaps_(UINT *caps, UINT n) { (void)n; caps[0] = 1; caps[1] = 1000000; return 0; }
static DWORD WINAPI timeGetTime_(void) { return rt_ticks_ms(); }

/* joystick (classic winmm API): real devices are exposed through DirectInput */
extern UINT joy_count(void);
extern UINT joy_caps(UINT id, uint8_t *caps, UINT n);
extern UINT joy_pos(UINT id, uint32_t *ji);
static UINT WINAPI joyGetNumDevs_(void) { return 16; }
static UINT WINAPI joyGetDevCapsA_(UINT id, uint8_t *caps, UINT n) { return joy_caps(id, caps, n); }
static UINT WINAPI joyGetPosEx_(UINT id, uint32_t *ji) { return joy_pos(id, ji); }

/* ------------------------------------------------------------------ */
/* ADVAPI32: tiny registry (nothing is stored: GP3 only probes DirectX keys) */
static LONG WINAPI RegOpenKeyExA_(uint32_t k, const char *sub, DWORD o, DWORD sam, uint32_t *out) {
    (void)o; (void)sam; (void)out;
    LOG("RegOpenKeyExA(%x, %s) -> not found", k, sub ? sub : "");
    return 2;
}
static LONG WINAPI RegCreateKeyExA_(uint32_t k, const char *sub, DWORD r, char *cls, DWORD o, DWORD sam, void *sa, uint32_t *out, DWORD *disp) {
    (void)r; (void)cls; (void)o; (void)sam; (void)sa;
    LOG("RegCreateKeyExA(%x, %s) -> in-memory key", k, sub ? sub : ""); if (out) *out = 0x4b4b0001; if (disp) *disp = 2; return 0;
}
static LONG WINAPI RegSetValueExA_(uint32_t k, const char *n, DWORD r, DWORD t, const uint8_t *d, DWORD sz) {
    (void)k; (void)r; LOG("RegSetValueExA(%s, type %u) = %.*s", n ? n : "", t, (int)sz, t == 1 ? (const char *)d : ""); return 0;
}
static LONG WINAPI RegQueryValueExA_(uint32_t k, const char *n, DWORD *r, DWORD *t, uint8_t *d, DWORD *sz) {
    (void)k; (void)n; (void)r; (void)t; (void)d; (void)sz; return 2;
}
static LONG WINAPI RegCloseKey_(uint32_t k) { (void)k; return 0; }

/* ------------------------------------------------------------------ */
/* OLE32                                                                */
static HRESULT WINAPI CoInitialize_(void *p) { (void)p; return 0; }
static void WINAPI CoUninitialize_(void) {}
extern HRESULT dplay_create(const uint8_t *clsid, const uint8_t *iid, void **out);
static HRESULT WINAPI CoCreateInstance_(const uint8_t *clsid, void *outer, DWORD ctx, const uint8_t *iid, void **out) {
    (void)outer; (void)ctx;
    return dplay_create(clsid, iid, out);
}

/* ------------------------------------------------------------------ */
/* WINSPOOL: no printers                                                */
static BOOL WINAPI EnumPrintersA_(DWORD fl, char *name, DWORD lvl, void *buf, DWORD cb, DWORD *need, DWORD *ret) {
    (void)fl; (void)name; (void)lvl; (void)buf; (void)cb;
    if (need) *need = 0; if (ret) *ret = 0; return TRUE;
}
static BOOL WINAPI OpenPrinterA_(char *n, HANDLE *h, void *d) { (void)n; (void)d; if (h) *h = 0; return FALSE; }
static BOOL WINAPI ClosePrinter_(HANDLE h) { (void)h; return TRUE; }
static LONG WINAPI DocumentPropertiesA_(HWND h, HANDLE p, char *n, void *o, void *i, DWORD m) { (void)h; (void)p; (void)n; (void)o; (void)i; (void)m; return -1; }
static BOOL WINAPI PrinterProperties_(HWND h, HANDLE p) { (void)h; (void)p; return FALSE; }

/* ------------------------------------------------------------------ */
/* WSOCK32: local name only (network play goes through DirectPlay)     */
static int WINAPI WSAStartup_(WORD ver, uint8_t *d) {
    memset(d, 0, 400); *(WORD *)d = ver; *(WORD *)(d + 2) = 0x0202; strcpy((char *)d + 4, "gp3rt sockets"); return 0;
}
static int WINAPI WSACleanup_(void) { return 0; }
static int WINAPI gethostname_(char *n, int l) { snprintf(n, l, "gp3"); return 0; }
static void *WINAPI gethostbyname_(const char *n) { (void)n; return NULL; }

/* ------------------------------------------------------------------ */
/* binkw32 fallback (used when the real binkw32.dll is not loaded): no movies */
static void *WINAPI BinkOpen_(const char *n, DWORD fl) { LOG("BinkOpen(%s) -> movies disabled", n); (void)fl; return NULL; }
static void WINAPI BinkClose_(void *b) { (void)b; }
static int WINAPI BinkDoFrame_(void *b) { (void)b; return 0; }
static void WINAPI BinkNextFrame_(void *b) { (void)b; }
static int WINAPI BinkWait_(void *b) { (void)b; return 0; }
static int WINAPI BinkCopyToBuffer_(void *b, void *d, int p, UINT h, UINT x, UINT y, UINT f) { (void)b; (void)d; (void)p; (void)h; (void)x; (void)y; (void)f; return 0; }
static void WINAPI BinkSetVolume_(void *b, int v) { (void)b; (void)v; }
static int WINAPI BinkSetSoundSystem_(void *o, DWORD p) { (void)o; (void)p; return 0; }
static void *WINAPI BinkOpenDirectSound_(DWORD p) { (void)p; return NULL; }
static int WINAPI BinkDDSurfaceType_(void *s) { (void)s; return 10; }

/* EAX.DLL (GP3 2000): EAXDirectSoundCreate fails, so the game falls back to plain DirectSoundCreate */
static HRESULT WINAPI EAXDirectSoundCreate_(void *g, void **ds, void *o) { (void)g; (void)o; if (ds) *ds = NULL; return (HRESULT)0x80004005; }
#define E(d, n) { d, #n, 0, (void *)n##_ }
static const export_t misc_exports[] = {
    E("winmm", timeSetEvent), E("winmm", timeKillEvent), E("winmm", timeBeginPeriod), E("winmm", timeEndPeriod),
    E("winmm", timeGetDevCaps), E("winmm", timeGetTime), E("winmm", joyGetNumDevs), E("winmm", joyGetDevCapsA),
    E("winmm", joyGetPosEx),
    E("advapi32", RegOpenKeyExA), E("advapi32", RegQueryValueExA), E("advapi32", RegCloseKey), E("advapi32", RegCreateKeyExA), E("advapi32", RegSetValueExA),
    E("ole32", CoInitialize), E("ole32", CoUninitialize), E("ole32", CoCreateInstance),
    E("winspool", EnumPrintersA), E("winspool", OpenPrinterA), E("winspool", ClosePrinter),
    E("winspool", DocumentPropertiesA), E("winspool", PrinterProperties),
    { "eax", "EAXDirectSoundCreate", 5, (void *)EAXDirectSoundCreate_ },
    E("wsock32", WSAStartup), E("wsock32", WSACleanup), E("wsock32", gethostname), E("wsock32", gethostbyname),
    { NULL, NULL, 0, NULL }
};
static const export_t bink_exports[] = {
    { "binkw32", "_BinkOpen@8", 0, (void *)BinkOpen_ }, { "binkw32", "_BinkClose@4", 0, (void *)BinkClose_ },
    { "binkw32", "_BinkDoFrame@4", 0, (void *)BinkDoFrame_ }, { "binkw32", "_BinkNextFrame@4", 0, (void *)BinkNextFrame_ },
    { "binkw32", "_BinkWait@4", 0, (void *)BinkWait_ }, { "binkw32", "_BinkCopyToBuffer@28", 0, (void *)BinkCopyToBuffer_ },
    { "binkw32", "_BinkSetVolume@8", 0, (void *)BinkSetVolume_ }, { "binkw32", "_BinkSetSoundSystem@8", 0, (void *)BinkSetSoundSystem_ },
    { "binkw32", "_BinkOpenDirectSound@4", 0, (void *)BinkOpenDirectSound_ }, { "binkw32", "_BinkDDSurfaceType@4", 0, (void *)BinkDDSurfaceType_ },
    { NULL, NULL, 0, NULL }
};
#undef E

extern void kernel32_register(void), user32_register(void), gdi32_register(void), ddraw_register(void),
            dinput_register(void), dsound_register(void);
int g_use_real_bink;
int g_native_ddraw;   /* load the Windows build of gp3rt ddraw.dll from the game folder (test harness) */
extern void harness_register(void);
extern void gpx_api_register(void);
void win32_register_all(void) {
    gpx_api_register();   /* first: overrides the simpler msvcrt pieces (stdio on _iob) */
    kernel32_register(); user32_register(); gdi32_register();
    rt_register_exports(misc_exports);
    harness_register();
    if (!g_use_real_bink) rt_register_exports(bink_exports);
    if (!g_native_ddraw) ddraw_register();
    dinput_register(); dsound_register();
}
