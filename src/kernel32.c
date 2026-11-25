/* KERNEL32 implementation for Grand Prix 3 (and binkw32.dll). */
#include "rt.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <malloc.h>
#include <ctype.h>
#include <fnmatch.h>

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NO_MORE_FILES 18
#define ERROR_FILE_EXISTS 80
#define ERROR_ALREADY_EXISTS 183
#define ERROR_ENVVAR_NOT_FOUND 203

/* ------------------------------------------------------------------ */
/* command line / current directory                                    */
static char g_cmdline[2048];
static char g_cwd[260] = "C:\\GP3";

void win32_set_cmdline(const char *s) { snprintf(g_cmdline, sizeof g_cmdline, "%s", s); }

/* make an absolute Windows path */
static void full_path(const char *in, char *out, size_t n) {
    if (!in) { out[0] = 0; return; }
    if ((isalpha((unsigned char)in[0]) && in[1] == ':') || in[0] == '\\' || in[0] == '/') {
        if (in[0] == '\\' || in[0] == '/') snprintf(out, n, "C:%s", in);
        else snprintf(out, n, "%s", in);
    } else snprintf(out, n, "%s\\%s", g_cwd, in);
}
int win_host_path(const char *win, char *host, size_t n) {
    char fp[1024]; full_path(win, fp, sizeof fp);
    return path_to_host(fp, host, n);
}

/* ------------------------------------------------------------------ */
/* errors / process                                                     */
static DWORD WINAPI GetLastError_(void) { teb_t *t = teb_current(); return t ? t->last_error : 0; }
static void WINAPI SetLastError_(DWORD e) { set_last_error(e); }
void set_last_error_ext(DWORD e) { set_last_error(e); }
static UINT WINAPI SetErrorMode_(UINT m) { static UINT old; UINT o = old; old = m; return o; }
static void WINAPI ExitProcess_(UINT code) {
    LOG("ExitProcess(%u)", code);
    if (g_log) fflush(g_log);
    fflush(stdout); fflush(stderr);
    _exit((int)code);
}
static HANDLE WINAPI CreateToolhelp32Snapshot_(DWORD f, DWORD pid) { (void)f; (void)pid; return INVALID_HANDLE_VALUE; }
static BOOL WINAPI Process32First_(HANDLE h, void *pe) { (void)h; (void)pe; return FALSE; }
static BOOL WINAPI Process32Next_(HANDLE h, void *pe) { (void)h; (void)pe; return FALSE; }
static HANDLE WINAPI OpenProcess_(DWORD a, BOOL i, DWORD pid) { (void)a; (void)i; (void)pid; return 0; }
static BOOL WINAPI QueryFullProcessImageNameA_(HANDLE h, DWORD f, char *b, DWORD *n) { (void)h; (void)f; (void)b; (void)n; return FALSE; }
static BOOL WINAPI TerminateProcess_(HANDLE h, UINT code) {
    if (h == 0xffffffffu) ExitProcess_(code);
    return FALSE;
}
static HANDLE WINAPI GetCurrentProcess_(void) { return 0xffffffffu; }
static HANDLE WINAPI GetCurrentThread_(void) { return 0xfffffffeu; }
static DWORD WINAPI GetCurrentThreadId_(void) { teb_t *t = teb_current(); return t ? t->tid : 1; }
static DWORD WINAPI GetCurrentProcessId_(void) { teb_t *t = teb_current(); return t ? t->pid : 1; }
static DWORD WINAPI GetVersion_(void) { return 0x0A280105; /* 5.1 build 2600, NT */ }
static BOOL WINAPI GetVersionExA_(uint8_t *p) {
    DWORD sz = *(DWORD *)p;
    *(DWORD *)(p + 4) = 5; *(DWORD *)(p + 8) = 1; *(DWORD *)(p + 12) = 2600; *(DWORD *)(p + 16) = 2;
    memset(p + 20, 0, 128); strcpy((char *)p + 20, "Service Pack 3");
    if (sz >= 156) { *(WORD *)(p + 148) = 3; *(WORD *)(p + 150) = 0; *(WORD *)(p + 152) = 0x100; p[154] = 1; p[155] = 0; }
    return TRUE;
}
static char *WINAPI GetCommandLineA_(void) { return g_cmdline; }
static void WINAPI GetStartupInfoA_(uint8_t *p) { memset(p, 0, 68); *(DWORD *)p = 68; }
static void WINAPI OutputDebugStringA_(const char *s) {
    if (!s) return;
    char b[512]; size_t n = strlen(s); if (n >= sizeof b) n = sizeof b - 1;
    memcpy(b, s, n); b[n] = 0;
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    LOG("dbg: %s", b);
}
static void WINAPI DebugBreak_(void) { WARN("DebugBreak called"); }
static uint32_t g_unhandled_filter;
static uint32_t WINAPI SetUnhandledExceptionFilter_(uint32_t f) { uint32_t o = g_unhandled_filter; g_unhandled_filter = f; return o; }
void gpx_dump_modules(void);
static LONG WINAPI UnhandledExceptionFilter_(void *p) { gpx_dump_modules(); uint32_t *ep = p, *er = ep ? (uint32_t *)(uintptr_t)ep[0] : NULL, *cx = ep ? (uint32_t *)(uintptr_t)ep[1] : NULL; WARN("UnhandledExceptionFilter called: code %08x at %08x (addr %08x) eax=%08x ecx=%08x edx=%08x esi=%08x edi=%08x", er ? er[0] : 0, er ? er[3] : 0, er && er[4] > 1 ? er[6] : 0, cx ? cx[44] : 0, cx ? cx[43] : 0, cx ? cx[42] : 0, cx ? cx[40] : 0, cx ? cx[39] : 0); return 1; }
static void WINAPI RaiseException_(DWORD code, DWORD flags, DWORD n, const uint32_t *args) {
    WARN("RaiseException %08x", code); (void)flags; (void)n; (void)args;
    _exit(5);
}

/* ------------------------------------------------------------------ */
/* system info                                                          */
static void WINAPI GetSystemInfo_(uint8_t *p) {
    memset(p, 0, 36);
    *(DWORD *)(p + 4) = 4096;
    *(DWORD *)(p + 8) = 0x10000;
    *(DWORD *)(p + 12) = 0x7ffeffff;
    *(DWORD *)(p + 16) = 1;
    *(DWORD *)(p + 20) = 1;
    *(DWORD *)(p + 24) = 586;
    *(DWORD *)(p + 28) = 0x10000;
    *(WORD *)(p + 32) = 6;
    *(WORD *)(p + 34) = 0x0801;
}
static void WINAPI GlobalMemoryStatus_(DWORD *p) {
    p[0] = 32; p[1] = 30;
    p[2] = 1024u << 20; p[3] = 768u << 20;     /* phys total/avail: 1GB is "plenty" for 2000-era code */
    p[4] = 2048u << 20; p[5] = 1536u << 20;    /* page file */
    p[6] = 0x7ffe0000u; p[7] = 0x60000000u;    /* virtual */
}
static UINT WINAPI GetDriveTypeA_(const char *root) {
    if (!root) return 3;
    char c = (char)tolower((unsigned char)root[0]);
    return c == 'c' ? 3 /* FIXED */ : c == VIRTUAL_CD_DRIVE ? 5 /* CDROM */ : 1 /* NO_ROOT_DIR */;
}
static DWORD WINAPI GetLogicalDrives_(void) { return (1u << 2) | (1u << (VIRTUAL_CD_DRIVE - 'a')); }
static BOOL WINAPI GetVolumeInformationA_(const char *root, char *name, DWORD nl, DWORD *serial, DWORD *maxc, DWORD *flags, char *fs, DWORD fl) {
    int drv = root ? tolower((unsigned char)root[0]) : 0;
    if (drv != 'c' && drv != VIRTUAL_CD_DRIVE) { set_last_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    if (name && nl) snprintf(name, nl, drv == 'c' ? "SYSTEM" : "GP3");
    if (serial) *serial = 0x19980701;
    if (maxc) *maxc = 255;
    if (flags) *flags = 3;
    if (fs && fl) snprintf(fs, fl, "NTFS");
    return TRUE;
}
static UINT WINAPI GetSystemDirectoryA_(char *b, UINT n) { snprintf(b, n, "C:\\WINDOWS\\SYSTEM32"); return 19; }
static UINT WINAPI GetWindowsDirectoryA_(char *b, UINT n) { snprintf(b, n, "C:\\WINDOWS"); return 10; }
static BOOL WINAPI IsProcessorFeaturePresent_(DWORD f) { return f == 0 /*FP errata*/ ? FALSE : (f <= 10); }

/* ------------------------------------------------------------------ */
/* time                                                                 */
#define QPC_FREQ 1193182ull   /* the frequency GP3 hard-codes in its timing code */
static BOOL WINAPI QueryPerformanceFrequency_(uint64_t *f) { *f = QPC_FREQ; return TRUE; }
static BOOL WINAPI QueryPerformanceCounter_(uint64_t *c) {
    uint64_t us = rt_now_us();
    *c = us * QPC_FREQ / 1000000ull;
    return TRUE;
}
static void WINAPI Sleep_(DWORD ms) {
    if (!ms) { sched_yield(); return; }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
DWORD WINAPI k32_GetTickCount(void) { return rt_ticks_ms(); }
static void fill_systime(WORD *p, int local) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; time_t t = ts.tv_sec;
    if (local) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
    p[0] = tm.tm_year + 1900; p[1] = tm.tm_mon + 1; p[2] = tm.tm_wday; p[3] = tm.tm_mday;
    p[4] = tm.tm_hour; p[5] = tm.tm_min; p[6] = tm.tm_sec; p[7] = ts.tv_nsec / 1000000;
}
static void WINAPI GetLocalTime_(WORD *p) { fill_systime(p, 1); }
static void WINAPI GetSystemTime_(WORD *p) { fill_systime(p, 0); }
static DWORD WINAPI GetTimeZoneInformation_(uint8_t *p) { memset(p, 0, 172); return 0; }
static uint64_t unix_to_filetime(time_t t) { return ((uint64_t)t + 11644473600ull) * 10000000ull; }
static BOOL WINAPI FileTimeToDosDateTime_(const uint64_t *ft, WORD *date, WORD *tm_) {
    time_t t = (time_t)(*ft / 10000000ull - 11644473600ull);
    struct tm tm; localtime_r(&t, &tm);
    if (date) *date = (WORD)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    if (tm_) *tm_ = (WORD)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* memory                                                               */
typedef struct { uint32_t base, size; } valloc_t;
static valloc_t g_vallocs[4096];
static pthread_mutex_t g_vmx = PTHREAD_MUTEX_INITIALIZER;

static void *WINAPI VirtualAlloc_(void *addr, DWORD size, DWORD type, DWORD prot) {
    (void)prot;
    uint32_t a = (uint32_t)(uintptr_t)addr;
    pthread_mutex_lock(&g_vmx);
    if (a) {
        for (int i = 0; i < 4096; i++)
            if (g_vallocs[i].size && a >= g_vallocs[i].base && a < g_vallocs[i].base + g_vallocs[i].size) {
                pthread_mutex_unlock(&g_vmx);
                return addr;   /* commit inside an existing reservation */
            }
    }
    void *p = rt_valloc(a & ~0xffffu, size + (a & 0xffffu), 1);
    if (p) for (int i = 0; i < 4096; i++) if (!g_vallocs[i].size) { g_vallocs[i].base = (uint32_t)(uintptr_t)p; g_vallocs[i].size = (size + 0xfff) & ~0xfffu; break; }
    pthread_mutex_unlock(&g_vmx);
    TRACE("VirtualAlloc(%p, %x, %x) -> %p", addr, size, type, p);
    if (!p) set_last_error(8);
    return p && a ? addr : p;
}
static BOOL WINAPI VirtualFree_(void *addr, DWORD size, DWORD type) {
    if (!(type & 0x8000)) return TRUE; /* decommit: keep mapping */
    uint32_t a = (uint32_t)(uintptr_t)addr;
    pthread_mutex_lock(&g_vmx);
    for (int i = 0; i < 4096; i++)
        if (g_vallocs[i].size && g_vallocs[i].base == a) { munmap(addr, g_vallocs[i].size); g_vallocs[i].size = 0; break; }
    pthread_mutex_unlock(&g_vmx);
    (void)size;
    return TRUE;
}
static BOOL WINAPI VirtualProtect_(void *a, DWORD n, DWORD np, DWORD *old) {
    if (old) *old = 0x40;
    { static int k; uintptr_t ra = (uintptr_t)__builtin_return_address(0);
      if (getenv("GP3_GPX_TRACE") && k++ < 2000) LOG("VirtualProtect(%p, %u, %x) from %08x", a, n, np, (unsigned)ra); }
    int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    switch (np & 0xff) {
    case 0x01: prot = PROT_NONE; break;                       /* PAGE_NOACCESS */
    case 0x02: prot = PROT_READ; break;                       /* PAGE_READONLY */
    case 0x04: case 0x08: prot = PROT_READ | PROT_WRITE; break;
    case 0x10: case 0x20: prot = PROT_READ | PROT_EXEC; break;
    }
    uintptr_t s = (uintptr_t)a & ~0xfffu, e = ((uintptr_t)a + n + 0xfff) & ~0xfffu;
    return mprotect((void *)s, e - s, prot) == 0;
}
/* vectored exception handlers (dispatched by thread.c before frame-based SEH) */
uint32_t g_veh[8]; int g_nveh;
static void *WINAPI AddVectoredExceptionHandler_(DWORD first, uint32_t h) {
    if (g_nveh < 8) { if (first) { memmove(g_veh + 1, g_veh, g_nveh * 4); g_veh[0] = h; } else g_veh[g_nveh] = h; g_nveh++; }
    LOG("AddVectoredExceptionHandler(%p)", (void *)(uintptr_t)h);
    return (void *)(uintptr_t)h;
}
static BOOL WINAPI DisableThreadLibraryCalls_(HMODULE h) { (void)h; return TRUE; }
static BOOL WINAPI IsDebuggerPresent_(void) { return FALSE; }
/* GetWriteWatch emulation for testing: a snapshot per region, pages that differ count as written */
static struct { uint8_t *base; size_t len; uint8_t *snap; } g_ww[8];
static UINT WINAPI GetWriteWatch_(DWORD flags, void *base, size_t len, void **addrs, uintptr_t *count, DWORD *gran) {
    int i; for (i = 0; i < 8 && g_ww[i].base && (g_ww[i].base != base || g_ww[i].len != len); i++) {}
    if (i == 8) return 1;
    if (!g_ww[i].base) { g_ww[i].base = base; g_ww[i].len = len; g_ww[i].snap = calloc(len, 1); }
    size_t n = 0, cap = *count;
    for (size_t o = 0; o < len; o += 4096) {
        size_t m = len - o < 4096 ? len - o : 4096;
        if (memcmp(g_ww[i].base + o, g_ww[i].snap + o, m)) { if (n < cap) addrs[n] = g_ww[i].base + o; n++; if (flags & 1) memcpy(g_ww[i].snap + o, g_ww[i].base + o, m); }
    }
    *count = n < cap ? n : cap; if (gran) *gran = 4096;
    return 0;
}
static size_t WINAPI VirtualQuery_(const void *a, uint32_t *mbi, size_t n) {
    extern const char *rt_module_name_of(uint32_t a, uint32_t *base);
    if (n < 28) return 0;
    uint32_t base = 0; const char *nm = rt_module_name_of((uint32_t)(uintptr_t)a, &base);
    memset(mbi, 0, 28);
    mbi[0] = (uint32_t)(uintptr_t)a & ~4095u;           /* BaseAddress */
    mbi[1] = nm ? base : 0;                              /* AllocationBase */
    mbi[2] = 0x40; mbi[3] = 4096; mbi[4] = 0x1000;       /* AllocationProtect, RegionSize, MEM_COMMIT */
    mbi[5] = 0x20;                                       /* PAGE_EXECUTE_READ */
    mbi[6] = nm ? 0x1000000 : 0x20000;                   /* MEM_IMAGE / MEM_PRIVATE */
    return 28;
}

static HANDLE WINAPI HeapCreate_(DWORD f, DWORD i, DWORD m) { (void)f; (void)i; (void)m; return 0x00150000; }
static BOOL WINAPI HeapDestroy_(HANDLE h) { (void)h; return TRUE; }
static HANDLE WINAPI GetProcessHeap_(void) { return 0x00150000; }
static void *WINAPI HeapAlloc_(HANDLE h, DWORD fl, DWORD n) {
    (void)h;
    void *p = (fl & 8) ? calloc(1, n ? n : 1) : malloc(n ? n : 1);
    return p;
}
static BOOL WINAPI HeapFree_(HANDLE h, DWORD fl, void *p) { (void)h; (void)fl; free(p); return TRUE; }
static void *WINAPI HeapReAlloc_(HANDLE h, DWORD fl, void *p, DWORD n) {
    (void)h;
    size_t old = p ? malloc_usable_size(p) : 0;
    if (fl & 0x10) return old >= n ? p : NULL;   /* in-place only */
    void *q = realloc(p, n ? n : 1);
    if (q && (fl & 8) && n > old) memset((char *)q + old, 0, n - old);
    return q;
}
static DWORD WINAPI HeapSize_(HANDLE h, DWORD fl, void *p) { (void)h; (void)fl; return p ? (DWORD)malloc_usable_size(p) : 0xffffffffu; }
static void *WINAPI GlobalAlloc_(UINT fl, DWORD n) { return calloc(1, n ? n : 1); (void)fl; }
static void *WINAPI GlobalFree_(void *p) { free(p); return NULL; }
static void *WINAPI GlobalLock_(void *p) { return p; }
static BOOL WINAPI GlobalUnlock_(void *p) { (void)p; return TRUE; }
static void *WINAPI GlobalHandle_(void *p) { return p; }

static int ptr_ok(const void *p, DWORD n) {
    if (!p) return 0;
    uintptr_t a = (uintptr_t)p & ~0xfffu, e = ((uintptr_t)p + (n ? n : 1) + 0xfff) & ~0xfffu;
    unsigned char v;
    for (; a < e; a += 0x1000) if (syscall(SYS_mincore, a, 0x1000, &v) != 0) return 0;
    return 1;
}
static BOOL WINAPI IsBadReadPtr_(const void *p, DWORD n) { return !ptr_ok(p, n); }
static BOOL WINAPI IsBadWritePtr_(void *p, DWORD n) { return !ptr_ok(p, n); }
static BOOL WINAPI IsBadCodePtr_(void *p) { return !ptr_ok(p, 1); }

/* ------------------------------------------------------------------ */
/* TLS / sync                                                           */
static uint64_t g_tls_used;
static pthread_mutex_t g_tlsmx = PTHREAD_MUTEX_INITIALIZER;
static DWORD WINAPI TlsAlloc_(void) {
    pthread_mutex_lock(&g_tlsmx);
    for (int i = 0; i < 64; i++) if (!(g_tls_used & (1ull << i))) { g_tls_used |= 1ull << i; pthread_mutex_unlock(&g_tlsmx); return i; }
    pthread_mutex_unlock(&g_tlsmx);
    return 0xffffffffu;
}
static BOOL WINAPI TlsFree_(DWORD i) { if (i < 64) g_tls_used &= ~(1ull << i); return TRUE; }
static void *WINAPI TlsGetValue_(DWORD i) {
    teb_t *t = teb_current();
    if (i >= 64 || !t) return NULL;
    t->last_error = 0;
    return (void *)(uintptr_t)t->tls_slots[i];
}
static BOOL WINAPI TlsSetValue_(DWORD i, void *v) {
    teb_t *t = teb_current();
    if (i >= 64 || !t) return FALSE;
    t->tls_slots[i] = (uint32_t)(uintptr_t)v;
    return TRUE;
}

/* CRITICAL_SECTION: we keep a pthread recursive mutex pointer in LockSemaphore (+16) */
static pthread_mutex_t g_csinit = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *cs_get(uint32_t *cs) {
    pthread_mutex_t *m = (pthread_mutex_t *)(uintptr_t)__atomic_load_n(&cs[4], __ATOMIC_ACQUIRE);
    if (m) return m;
    pthread_mutex_lock(&g_csinit);
    m = (pthread_mutex_t *)(uintptr_t)cs[4];
    if (!m) {
        m = malloc(sizeof *m);
        pthread_mutexattr_t a; pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &a);
        __atomic_store_n(&cs[4], (uint32_t)(uintptr_t)m, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_csinit);
    return m;
}
static void WINAPI InitializeCriticalSection_(uint32_t *cs) { memset(cs, 0, 24); cs[1] = 0xffffffffu; cs_get(cs); }
static void WINAPI DeleteCriticalSection_(uint32_t *cs) {
    pthread_mutex_t *m = (pthread_mutex_t *)(uintptr_t)cs[4];
    if (m) { pthread_mutex_destroy(m); free(m); }
    cs[4] = 0;
}
static void WINAPI EnterCriticalSection_(uint32_t *cs) {
    pthread_mutex_lock(cs_get(cs));
    cs[2]++; cs[3] = GetCurrentThreadId_();
}
static void WINAPI LeaveCriticalSection_(uint32_t *cs) {
    if (--cs[2] == 0) cs[3] = 0;
    pthread_mutex_unlock(cs_get(cs));
}
static uint32_t g_aff = 1;   /* harness: start "pinned to one core" to exercise the DLL's fix */
static BOOL WINAPI GetProcessAffinityMask_(HANDLE h, uint32_t *pm, uint32_t *sm) { (void)h; if (pm) *pm = g_aff; if (sm) *sm = 0xf; return TRUE; }
static BOOL WINAPI SetProcessAffinityMask_(HANDLE h, uint32_t m) { (void)h; g_aff = m; return TRUE; }
static BOOL WINAPI TryEnterCriticalSection_(uint32_t *cs) {
    if (pthread_mutex_trylock(cs_get(cs))) return FALSE;
    cs[2]++; cs[3] = GetCurrentThreadId_(); return TRUE;
}
static HANDLE WINAPI OpenThread_(DWORD a, BOOL i, DWORD id) { (void)a; (void)i; (void)id; return 0; }
static DWORD WINAPI SuspendThread_(HANDLE h) { (void)h; return (DWORD)-1; }
static BOOL WINAPI GetThreadContext_(HANDLE h, void *c) { (void)h; (void)c; return FALSE; }
static LONG WINAPI InterlockedIncrement_(LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static LONG WINAPI InterlockedDecrement_(LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static LONG WINAPI InterlockedExchange_(LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }

/* waitable objects: events, semaphores, threads */
typedef struct {
    pthread_mutex_t mx; pthread_cond_t cv;
    int manual, signaled;      /* event */
    LONG count, max;           /* semaphore */
    int done; DWORD exit_code; pthread_t th; int suspended; /* thread */
    char name[64];
} waitobj_t;

static waitobj_t *wo_new(void) {
    waitobj_t *w = calloc(1, sizeof *w);
    pthread_mutex_init(&w->mx, NULL);
    pthread_condattr_t ca; pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&w->cv, &ca);
    return w;
}
static void wo_close(hobj_t *o) { (void)o; /* objects may still be waited on; leak deliberately */ }

static HANDLE WINAPI CreateEventA_(void *sa, BOOL manual, BOOL init, const char *name) {
    (void)sa;
    waitobj_t *w = wo_new(); w->manual = manual; w->signaled = init;
    if (name) snprintf(w->name, sizeof w->name, "%s", name);
    return handle_new(H_EVENT, w, wo_close);
}
static HANDLE WINAPI CreateSemaphoreA_(void *sa, LONG init, LONG max, const char *name) {
    (void)sa;
    waitobj_t *w = wo_new(); w->count = init; w->max = max;
    if (name) snprintf(w->name, sizeof w->name, "%s", name);
    set_last_error(0);
    return handle_new(H_SEMAPHORE, w, wo_close);
}
static HANDLE WINAPI CreateMutexA_(void *sa, BOOL own, const char *name) {
    (void)sa; (void)name;
    waitobj_t *w = wo_new(); w->count = own ? 0 : 1; w->max = 1;
    return handle_new(H_SEMAPHORE, w, wo_close);
}
static BOOL WINAPI SetEvent_(HANDLE h) {
    hobj_t *o = handle_get(h, H_EVENT); if (!o) return FALSE;
    waitobj_t *w = o->p;
    pthread_mutex_lock(&w->mx); w->signaled = 1; pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->mx);
    return TRUE;
}
static BOOL WINAPI ResetEvent_(HANDLE h) {
    hobj_t *o = handle_get(h, H_EVENT); if (!o) return FALSE;
    waitobj_t *w = o->p;
    pthread_mutex_lock(&w->mx); w->signaled = 0; pthread_mutex_unlock(&w->mx);
    return TRUE;
}
static BOOL WINAPI ReleaseSemaphore_(HANDLE h, LONG n, LONG *prev) {
    hobj_t *o = handle_get(h, H_SEMAPHORE); if (!o) return FALSE;
    waitobj_t *w = o->p;
    pthread_mutex_lock(&w->mx);
    if (prev) *prev = w->count;
    w->count += n; if (w->max && w->count > w->max) w->count = w->max;
    pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->mx);
    return TRUE;
}
static BOOL WINAPI ReleaseMutex_(HANDLE h) { return ReleaseSemaphore_(h, 1, NULL); }
/* waitable timers (one-shot, relative due time) */
typedef struct { uint64_t due_us; } wtimer_t;
static HANDLE WINAPI CreateWaitableTimerA_(void *sa, BOOL manual, const char *name) { (void)sa; (void)manual; (void)name; return handle_new(H_EVENT + 100, calloc(1, sizeof(wtimer_t)), NULL); }
static BOOL WINAPI SetWaitableTimer_(HANDLE h, const int64_t *due, LONG period, void *cb, void *arg, BOOL resume) {
    (void)period; (void)cb; (void)arg; (void)resume;
    hobj_t *o = handle_get(h, H_EVENT + 100); if (!o) return FALSE;
    int64_t d = *due;
    ((wtimer_t *)o->p)->due_us = d < 0 ? rt_now_us() + (uint64_t)(-d) / 10 : rt_now_us();
    return TRUE;
}

DWORD WINAPI k32_WaitForSingleObject(HANDLE h, DWORD ms) {
    hobj_t *o = handle_get(h, 0);
    if (o && o->type == H_EVENT + 100) {
        uint64_t now = rt_now_us(), due = ((wtimer_t *)o->p)->due_us;
        if (due > now) { struct timespec ts = { (time_t)((due - now) / 1000000), (long)((due - now) % 1000000) * 1000 }; nanosleep(&ts, NULL); }
        return 0;
    }
    if (!o || (o->type != H_EVENT && o->type != H_SEMAPHORE && o->type != H_THREAD)) return 0xffffffffu;
    waitobj_t *w = o->p;
    struct timespec dl; clock_gettime(CLOCK_MONOTONIC, &dl);
    if (ms != 0xffffffffu) { dl.tv_sec += ms / 1000; dl.tv_nsec += (ms % 1000) * 1000000L; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; } }
    pthread_mutex_lock(&w->mx);
    for (;;) {
        int ready = o->type == H_EVENT ? w->signaled : o->type == H_SEMAPHORE ? w->count > 0 : w->done;
        if (ready) {
            if (o->type == H_EVENT && !w->manual) w->signaled = 0;
            if (o->type == H_SEMAPHORE) w->count--;
            pthread_mutex_unlock(&w->mx);
            return 0;
        }
        if (ms == 0) break;
        if (ms == 0xffffffffu) pthread_cond_wait(&w->cv, &w->mx);
        else if (pthread_cond_timedwait(&w->cv, &w->mx, &dl) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&w->mx);
    return 0x102; /* WAIT_TIMEOUT */
}
static DWORD WINAPI WaitForSingleObject_(HANDLE h, DWORD ms) { return k32_WaitForSingleObject(h, ms); }

/* threads */
typedef DWORD (WINAPI *thread_fn)(void *);
extern int rt_thread_create(thread_fn fn, void *arg, DWORD *tid, pthread_t *out);
struct thr_start { thread_fn fn; void *arg; waitobj_t *w; };
static DWORD WINAPI thread_wrapper(void *p) {
    struct thr_start s = *(struct thr_start *)p; free(p);
    pthread_mutex_lock(&s.w->mx);
    while (s.w->suspended) pthread_cond_wait(&s.w->cv, &s.w->mx);
    pthread_mutex_unlock(&s.w->mx);
    DWORD r = s.fn(s.arg);
    pthread_mutex_lock(&s.w->mx); s.w->done = 1; s.w->exit_code = r; pthread_cond_broadcast(&s.w->cv); pthread_mutex_unlock(&s.w->mx);
    return r;
}
static HANDLE WINAPI CreateThread_(void *sa, DWORD stack, thread_fn fn, void *arg, DWORD flags, DWORD *tid) {
    (void)sa; (void)stack;
    waitobj_t *w = wo_new();
    w->suspended = (flags & 4) != 0;
    struct thr_start *s = malloc(sizeof *s); s->fn = fn; s->arg = arg; s->w = w;
    HANDLE h = handle_new(H_THREAD, w, wo_close);
    if (rt_thread_create(thread_wrapper, s, tid, &w->th)) { WARN("CreateThread failed"); return 0; }
    LOG("CreateThread(fn=%p, flags=%x) -> %x", (void *)fn, flags, h);
    return h;
}
static DWORD WINAPI ResumeThread_(HANDLE h) {
    hobj_t *o = handle_get(h, H_THREAD); if (!o) return 0xffffffffu;
    waitobj_t *w = o->p;
    pthread_mutex_lock(&w->mx); DWORD prev = w->suspended; w->suspended = 0; pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->mx);
    return prev;
}
static void WINAPI ExitThread_(DWORD code) { pthread_exit((void *)(uintptr_t)code); }
static BOOL WINAPI GetExitCodeThread_(HANDLE h, DWORD *c) {
    hobj_t *o = handle_get(h, H_THREAD); if (!o) return FALSE;
    waitobj_t *w = o->p; *c = w->done ? w->exit_code : 259; return TRUE;
}
static BOOL WINAPI SetThreadPriority_(HANDLE h, int p) { (void)h; (void)p; return TRUE; }
static int WINAPI GetThreadPriority_(HANDLE h) { (void)h; return 0; }
static BOOL WINAPI CloseHandle_(HANDLE h) {
    if (h == 0xffffffffu || h == 0xfffffffeu) return TRUE;
    return handle_close(h);
}

/* ------------------------------------------------------------------ */
/* files                                                                */
typedef struct { int fd; int is_std; char path[1024]; } file_t;
static void file_close(hobj_t *o) { file_t *f = o->p; if (!f->is_std) close(f->fd); free(f); }

static HANDLE std_handles[3];
static HANDLE WINAPI GetStdHandle_(DWORD n) {
    int i = n == (DWORD)-10 ? 0 : n == (DWORD)-11 ? 1 : n == (DWORD)-12 ? 2 : -1;
    if (i < 0) return INVALID_HANDLE_VALUE;
    if (!std_handles[i]) {
        file_t *f = calloc(1, sizeof *f); f->fd = i; f->is_std = 1;
        std_handles[i] = handle_new(H_FILE, f, file_close);
    }
    return std_handles[i];
}
static BOOL WINAPI SetStdHandle_(DWORD n, HANDLE h) { (void)n; (void)h; return TRUE; }
static DWORD WINAPI GetFileType_(HANDLE h) {
    hobj_t *o = handle_get(h, H_FILE);
    if (!o) return 0;
    return ((file_t *)o->p)->is_std ? 2 : 1;
}
static UINT WINAPI SetHandleCount_(UINT n) { return n; }

/* GP3 1.13 language files (ldata\*.str) that predate 1.13 hold ~0x9e8 strings where the game
   (and GPxPatch, which rebuilds the string table) expect 0xa75: the game reads past the end and
   GPxPatch numbers its own strings wrongly (the "Software 3d" renderer name turned to garbage
   and the game asked to recalibrate on every start). Such files are served with the 1.13 count
   and empty strings for the missing entries; the file on disk is not changed. */
#include <sys/syscall.h>
static int str_file_fix(int fd, const char *hp) {
    size_t l = strlen(hp);
    if (l < 5 || strcasecmp(hp + l - 4, ".str")) return fd;
    struct stat st; if (fstat(fd, &st) || st.st_size < 8 || st.st_size > (1 << 22)) return fd;
    uint32_t cnt = 0; if (pread(fd, &cnt, 4, 0) != 4 || cnt < 0x800 || cnt >= 0xa75) return fd;
    uint8_t *d = malloc(st.st_size + 0x200);
    if (pread(fd, d, st.st_size, 0) != st.st_size) { free(d); return fd; }
    *(uint32_t *)d = 0xa75; memset(d + st.st_size, 0, 0x200);
    int mfd = (int)syscall(SYS_memfd_create, "gp3str", 0);
    if (mfd < 0) { free(d); return fd; }
    if (write(mfd, d, st.st_size + 0x200) != st.st_size + 0x200) { close(mfd); free(d); return fd; }
    lseek(mfd, 0, SEEK_SET); free(d); close(fd);
    LOG("language file %s: %u strings, served as the 1.13 table (0xa75)", hp, cnt);
    return mfd;
}
static HANDLE WINAPI CreateFileA_(const char *name, DWORD acc, DWORD share, void *sa, DWORD disp, DWORD attrs, HANDLE tmpl) {
    (void)share; (void)sa; (void)attrs; (void)tmpl;
    char hp[1024];
    int exists = win_host_path(name, hp, sizeof hp);
    int fl = 0;
    if ((acc & 0x40000000) && (acc & 0x80000000)) fl = O_RDWR;
    else if (acc & 0x40000000) fl = O_WRONLY;
    else fl = O_RDONLY;
    if (acc & 0x10000000) fl = O_RDWR; /* GENERIC_ALL */
    struct stat st;
    int is_dir = exists && !stat(hp, &st) && S_ISDIR(st.st_mode);
    switch (disp) {
    case 1: if (exists) { set_last_error(ERROR_FILE_EXISTS); return INVALID_HANDLE_VALUE; } fl |= O_CREAT | O_EXCL; break;
    case 2: fl |= O_CREAT | O_TRUNC; break;
    case 3: if (!exists) { set_last_error(ERROR_FILE_NOT_FOUND); TRACE("CreateFileA(%s) -> not found", name); return INVALID_HANDLE_VALUE; } break;
    case 4: fl |= O_CREAT; break;
    case 5: if (!exists) { set_last_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; } fl |= O_TRUNC; break;
    }
    if ((fl & O_CREAT) && (fl & O_ACCMODE) == O_RDONLY) fl = (fl & ~O_ACCMODE) | O_RDWR;
    if (is_dir) fl = O_RDONLY;
    int fd = open(hp, fl | O_CLOEXEC, 0644);
    if (fd >= 0 && (fl & O_ACCMODE) == O_RDONLY && !is_dir) fd = str_file_fix(fd, hp);
    if (fd < 0) {
        set_last_error(errno == ENOENT ? ERROR_PATH_NOT_FOUND : ERROR_ACCESS_DENIED);
        LOG("CreateFileA(%s, acc=%x, disp=%u) failed: %s", name, acc, disp, strerror(errno));
        return INVALID_HANDLE_VALUE;
    }
    file_t *f = calloc(1, sizeof *f); f->fd = fd; snprintf(f->path, sizeof f->path, "%s", hp);
    HANDLE h = handle_new(H_FILE, f, file_close);
    TRACE("CreateFileA(%s, acc=%x, disp=%u) -> %x", name, acc, disp, h);
    set_last_error((disp == 2 || disp == 4) && exists ? ERROR_ALREADY_EXISTS : 0);
    return h;
}
/* diagnostics: GP3_WATCH_READ=<name part>,<file offset>,<len>,<seconds>: when that part of the
 * file is read, log every instruction touching it in memory (out/acc.bin) for <seconds> */
static void *watch_off(void *a) { extern uint8_t *g_acc_base; extern size_t g_acc_len; int sec = (int)(intptr_t)a;
    sleep(sec); uint8_t *b = g_acc_base; size_t l = g_acc_len; g_acc_base = NULL; if (b) mprotect(b, l, PROT_READ | PROT_WRITE);
    extern FILE *g_acc_file; if (g_acc_file) fflush(g_acc_file); LOG("watch-read: done"); return NULL; }
static void watch_read(file_t *f, void *buf, off_t pos, size_t got) {
    static int done; const char *spec = getenv("GP3_WATCH_READ"); if (!spec || done) return;
    char part[256]; long off = 0, len = 0; int sec = 5;
    if (sscanf(spec, "%255[^,],%li,%li,%d", part, &off, &len, &sec) < 3) return;
    if (!strcasestr(f->path, part) || off < pos || off >= pos + (off_t)got) return;
    done = 1;
    extern uint8_t *g_acc_base; extern size_t g_acc_len; extern FILE *g_acc_file;
    uint8_t *a = (uint8_t *)buf + (off - pos);
    uint8_t *pg = (uint8_t *)((uintptr_t)a & ~(uintptr_t)4095); size_t l = (((uintptr_t)a + len + 4095) & ~(uintptr_t)4095) - (uintptr_t)pg;
    g_acc_file = fopen("/home/claude/gp3/out/acc.bin", "wb");
    LOG("watch-read: %s offset %lx is at %p", f->path, off, (void *)a);
    g_acc_len = l; g_acc_base = pg; mprotect(pg, l, PROT_NONE);
    pthread_t th; pthread_create(&th, NULL, watch_off, (void *)(intptr_t)sec);
}
static BOOL WINAPI ReadFile_(HANDLE h, void *buf, DWORD n, DWORD *nr, void *ov) {
    (void)ov;
    hobj_t *o = handle_get(h, H_FILE);
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); if (nr) *nr = 0; return FALSE; }
    file_t *f = o->p;
    off_t pos0 = lseek(f->fd, 0, SEEK_CUR);
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(f->fd, (char *)buf + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; if (nr) *nr = got; return FALSE; }
        if (r == 0) break;
        got += r;
    }
    if (nr) *nr = (DWORD)got;
    watch_read(f, buf, pos0, got);
    return TRUE;
}
static BOOL WINAPI WriteFile_(HANDLE h, const void *buf, DWORD n, DWORD *nw, void *ov) {
    (void)ov;
    hobj_t *o = handle_get(h, H_FILE);
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    file_t *f = o->p;
    if (f->is_std) { LOG("stdout: %.*s", (int)n, (const char *)buf); if (nw) *nw = n; return TRUE; }
    size_t put = 0;
    while (put < n) {
        ssize_t r = write(f->fd, (const char *)buf + put, n - put);
        if (r < 0) { if (errno == EINTR) continue; if (nw) *nw = put; return FALSE; }
        put += r;
    }
    if (nw) *nw = (DWORD)put;
    return TRUE;
}
static DWORD WINAPI SetFilePointer_(HANDLE h, LONG lo, LONG *hi, DWORD how) {
    hobj_t *o = handle_get(h, H_FILE);
    if (!o) { set_last_error(ERROR_INVALID_HANDLE); return 0xffffffffu; }
    int64_t off = hi ? ((int64_t)*hi << 32) | (uint32_t)lo : lo;
    off_t r = lseek(((file_t *)o->p)->fd, off, how == 0 ? SEEK_SET : how == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { set_last_error(131); return 0xffffffffu; }
    if (hi) *hi = (LONG)((int64_t)r >> 32);
    set_last_error(0);
    return (DWORD)r;
}
static DWORD WINAPI GetFileSize_(HANDLE h, DWORD *hi) {
    hobj_t *o = handle_get(h, H_FILE);
    if (!o) return 0xffffffffu;
    struct stat st; fstat(((file_t *)o->p)->fd, &st);
    if (hi) *hi = (DWORD)((uint64_t)st.st_size >> 32);
    return (DWORD)st.st_size;
}
static BOOL WINAPI SetEndOfFile_(HANDLE h) {
    hobj_t *o = handle_get(h, H_FILE); if (!o) return FALSE;
    file_t *f = o->p; off_t p = lseek(f->fd, 0, SEEK_CUR);
    return ftruncate(f->fd, p) == 0;
}
static BOOL WINAPI FlushFileBuffers_(HANDLE h) { (void)h; return TRUE; }
static BOOL WINAPI GetFileTime_(HANDLE h, uint64_t *c, uint64_t *a, uint64_t *w) {
    hobj_t *o = handle_get(h, H_FILE); if (!o) return FALSE;
    struct stat st; fstat(((file_t *)o->p)->fd, &st);
    if (c) *c = unix_to_filetime(st.st_mtime);
    if (a) *a = unix_to_filetime(st.st_atime);
    if (w) *w = unix_to_filetime(st.st_mtime);
    return TRUE;
}
static DWORD WINAPI GetFileAttributesA_(const char *name) {
    char hp[1024];
    if (!win_host_path(name, hp, sizeof hp)) { set_last_error(ERROR_FILE_NOT_FOUND); TRACE("GetFileAttributesA(%s) -> none", name); return 0xffffffffu; }
    struct stat st; if (stat(hp, &st)) return 0xffffffffu;
    DWORD a = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
    if (!(st.st_mode & 0200)) a |= 1;
    return a;
}
static BOOL WINAPI SetFileAttributesA_(const char *name, DWORD a) {
    char hp[1024];
    if (!win_host_path(name, hp, sizeof hp)) return FALSE;
    struct stat st; if (stat(hp, &st)) return FALSE;
    chmod(hp, (a & 1) ? (st.st_mode & ~0222) : (st.st_mode | 0200));
    return TRUE;
}
static BOOL WINAPI CreateDirectoryA_(const char *name, void *sa) {
    (void)sa; char hp[1024];
    if (win_host_path(name, hp, sizeof hp)) { set_last_error(ERROR_ALREADY_EXISTS); return FALSE; }
    return mkdir(hp, 0755) == 0;
}
static BOOL WINAPI DeleteFileA_(const char *name) {
    char hp[1024];
    if (!win_host_path(name, hp, sizeof hp)) { set_last_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    LOG("DeleteFileA(%s)", name);
    return unlink(hp) == 0;
}
static BOOL WINAPI MoveFileA_(const char *a, const char *b) {
    char ha[1024], hb[1024];
    if (!win_host_path(a, ha, sizeof ha)) { set_last_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    if (win_host_path(b, hb, sizeof hb)) { set_last_error(ERROR_ALREADY_EXISTS); return FALSE; }
    return rename(ha, hb) == 0;
}
static DWORD WINAPI GetCurrentDirectoryA_(DWORD n, char *buf) {
    size_t l = strlen(g_cwd);
    if (n <= l) return (DWORD)l + 1;
    memcpy(buf, g_cwd, l + 1);
    return (DWORD)l;
}
static BOOL WINAPI SetCurrentDirectoryA_(const char *p) {
    char fp[1024], hp[1024]; full_path(p, fp, sizeof fp);
    if (!path_to_host(fp, hp, sizeof hp)) { set_last_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    size_t l = strlen(fp); while (l > 3 && (fp[l - 1] == '\\' || fp[l - 1] == '/')) fp[--l] = 0;
    snprintf(g_cwd, sizeof g_cwd, "%s", fp);
    LOG("SetCurrentDirectory(%s)", g_cwd);
    return TRUE;
}

/* utf-8 -> cp1252 (Latin-1 subset) */
static void utf8_to_cp1252(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < n;) {
        unsigned c = *p;
        if (c < 0x80) { out[o++] = (char)c; p++; }
        else if ((c & 0xe0) == 0xc0 && p[1]) { unsigned u = ((c & 31) << 6) | (p[1] & 63); out[o++] = u < 256 ? (char)u : '?'; p += 2; }
        else if ((c & 0xf0) == 0xe0 && p[1] && p[2]) { out[o++] = '?'; p += 3; }
        else { out[o++] = '?'; p++; }
    }
    out[o] = 0;
}

typedef struct { DIR *d; char dir[1024]; char pat[260]; } find_t;
static void find_close(hobj_t *o) { find_t *f = o->p; if (f->d) closedir(f->d); free(f); }
static int find_next(find_t *f, uint8_t *fd) {
    struct dirent *e;
    while ((e = readdir(f->d))) {
        char cp[260]; utf8_to_cp1252(e->d_name, cp, sizeof cp);
        if (fnmatch(f->pat, cp, FNM_CASEFOLD) != 0 && !(!strcmp(f->pat, "*.*"))) continue;
        char full[2048]; snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        struct stat st; if (stat(full, &st)) continue;
        memset(fd, 0, 320);
        *(DWORD *)fd = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
        uint64_t ft = unix_to_filetime(st.st_mtime);
        memcpy(fd + 4, &ft, 8); memcpy(fd + 12, &ft, 8); memcpy(fd + 20, &ft, 8);
        *(DWORD *)(fd + 28) = 0; *(DWORD *)(fd + 32) = (DWORD)st.st_size;
        snprintf((char *)fd + 44, 260, "%s", cp);
        return 1;
    }
    return 0;
}
static HANDLE WINAPI FindFirstFileA_(const char *pattern, uint8_t *fd) {
    char fp[1024]; full_path(pattern, fp, sizeof fp);
    char *sl = strrchr(fp, '\\');
    find_t *f = calloc(1, sizeof *f);
    if (sl) { *sl = 0; snprintf(f->pat, sizeof f->pat, "%s", sl + 1); } else snprintf(f->pat, sizeof f->pat, "%s", fp);
    if (!path_to_host(fp, f->dir, sizeof f->dir) || !(f->d = opendir(f->dir))) {
        free(f); set_last_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE;
    }
    if (!find_next(f, fd)) { closedir(f->d); free(f); set_last_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return handle_new(H_FIND, f, find_close);
}
static BOOL WINAPI FindNextFileA_(HANDLE h, uint8_t *fd) {
    hobj_t *o = handle_get(h, H_FIND); if (!o) return FALSE;
    if (!find_next(o->p, fd)) { set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
    return TRUE;
}
static BOOL WINAPI FindClose_(HANDLE h) { return handle_close(h); }

/* ------------------------------------------------------------------ */
/* INI files                                                            */
static DWORD WINAPI GetPrivateProfileStringA_(const char *app, const char *key, const char *def, char *buf, DWORD n, const char *file) {
    char hp[1024];
    char fp[1024];
    if (file && !strchr(file, '\\') && !strchr(file, '/')) snprintf(fp, sizeof fp, "C:\\WINDOWS\\%s", file);
    else snprintf(fp, sizeof fp, "%s", file ? file : "");
    const char *dflt = def ? def : "";
    FILE *f = NULL;
    if (win_host_path(fp, hp, sizeof hp)) f = fopen(hp, "r");
    if (!f && file && win_host_path(file, hp, sizeof hp)) f = fopen(hp, "r");
    TRACE("GetPrivateProfileStringA([%s] %s, file=%s) %s", app ? app : "*", key ? key : "*", file, f ? "" : "(no file)");
    if (getenv("GP3_INI_TRACE")) LOG("INI %s|%s|%s|%s", file ? file : "", app ? app : "*", key ? key : "*", def ? def : "");
    char line[512]; int in = 0; DWORD o = 0;
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *s = line; while (isspace((unsigned char)*s)) s++;
            char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
            if (*s == '[') {
                char *r = strchr(s, ']'); if (r) *r = 0;
                in = app ? !strcasecmp(s + 1, app) : 0;
                if (!app && n > o + 1) { o += snprintf(buf + o, n - o, "%s", s + 1) + 1; }
                continue;
            }
            if (!in || *s == ';' || !*s) continue;
            char *eq = strchr(s, '='); if (!eq) continue;
            *eq = 0; char *k = s, *v = eq + 1;
            char *ke = eq; while (ke > k && isspace((unsigned char)ke[-1])) *--ke = 0;
            while (isspace((unsigned char)*v)) v++;
            if (!key) { if (n > o + 1) o += snprintf(buf + o, n - o, "%s", k) + 1; continue; }
            if (!strcasecmp(k, key)) { fclose(f); snprintf(buf, n, "%s", v); return (DWORD)strlen(buf); }
        }
        fclose(f);
    }
    if (!key || !app) { if (o < n) buf[o] = 0; return o; }
    snprintf(buf, n, "%s", dflt);
    return (DWORD)strlen(buf);
}
static UINT WINAPI GetPrivateProfileIntA_(const char *app, const char *key, int def, const char *file) {
    char b[64]; GetPrivateProfileStringA_(app, key, "", b, sizeof b, file);
    return b[0] ? (UINT)atoi(b) : (UINT)def;
}

/* ------------------------------------------------------------------ */
/* environment / strings                                                */
static char g_envblock[4] = {0, 0, 0, 0};
static WCHAR g_envblockw[4];
static char *WINAPI GetEnvironmentStrings_(void) { return g_envblock; }
static WCHAR *WINAPI GetEnvironmentStringsW_(void) { return g_envblockw; }
static BOOL WINAPI FreeEnvironmentStrings_(void *p) { (void)p; return TRUE; }
static DWORD WINAPI GetEnvironmentVariableA_(const char *n, char *b, DWORD sz) { (void)n; (void)b; (void)sz; set_last_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
static BOOL WINAPI SetEnvironmentVariableA_(const char *n, const char *v) { (void)n; (void)v; return TRUE; }

static const uint16_t cp1252_hi[32] = {0x20AC,0x81,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,0x2C6,0x2030,0x160,0x2039,0x152,0x8D,0x17D,0x8F,
                                       0x90,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x2DC,0x2122,0x161,0x203A,0x153,0x9D,0x17E,0x178};
static int WINAPI MultiByteToWideChar_(UINT cp, DWORD fl, const char *s, int n, WCHAR *d, int dn) {
    (void)cp; (void)fl;
    if (n < 0) n = (int)strlen(s) + 1;
    if (!dn) return n;
    int m = n < dn ? n : dn;
    for (int i = 0; i < m; i++) { unsigned c = (unsigned char)s[i]; d[i] = (c >= 0x80 && c < 0xa0) ? cp1252_hi[c - 0x80] : c; }
    return m;
}
static int WINAPI WideCharToMultiByte_(UINT cp, DWORD fl, const WCHAR *s, int n, char *d, int dn, const char *defc, BOOL *used) {
    (void)cp; (void)fl;
    if (n < 0) { n = 0; while (s[n]) n++; n++; }
    if (used) *used = FALSE;
    if (!dn) return n;
    int m = n < dn ? n : dn;
    for (int i = 0; i < m; i++) {
        unsigned c = s[i];
        if (c >= 256) {
            int k; for (k = 0; k < 32 && cp1252_hi[k] != c; k++) {}
            if (k < 32) c = 0x80 + k; else { c = defc ? (unsigned char)*defc : '?'; if (used) *used = TRUE; }
        }
        d[i] = (char)c;
    }
    return m;
}
static UINT WINAPI GetACP_(void) { return 1252; }
static UINT WINAPI GetOEMCP_(void) { return 850; }
static BOOL WINAPI GetCPInfo_(UINT cp, uint8_t *p) { (void)cp; memset(p, 0, 20); *(DWORD *)p = 1; p[4] = '?'; return TRUE; }
static BOOL WINAPI GetStringTypeA_(DWORD lc, DWORD t, const char *s, int n, WORD *out) {
    (void)lc;
    if (n < 0) n = (int)strlen(s) + 1;
    for (int i = 0; i < n; i++) {
        unsigned c = (unsigned char)s[i]; WORD r = 0;
        if (t == 1) {
            if (isupper(c)) r |= 1; if (islower(c)) r |= 2; if (isdigit(c)) r |= 4; if (isspace(c)) r |= 8;
            if (ispunct(c)) r |= 0x10; if (iscntrl(c)) r |= 0x20; if (c == ' ' || c == '\t') r |= 0x40;
            if (isxdigit(c)) r |= 0x80; if (isalpha(c)) r |= 0x100;
            if (c >= 0xc0 && c != 0xd7 && c != 0xf7) r |= 0x100 | (c < 0xe0 ? 1 : 2);
        }
        out[i] = r;
    }
    return TRUE;
}
static BOOL WINAPI GetStringTypeW_(DWORD t, const WCHAR *s, int n, WORD *out) {
    if (n < 0) { n = 0; while (s[n]) n++; n++; }
    for (int i = 0; i < n; i++) {
        unsigned c = s[i]; WORD r = 0;
        if (t == 1 && c < 128) {
            if (isupper(c)) r |= 1; if (islower(c)) r |= 2; if (isdigit(c)) r |= 4; if (isspace(c)) r |= 8;
            if (ispunct(c)) r |= 0x10; if (iscntrl(c)) r |= 0x20; if (c == ' ' || c == '\t') r |= 0x40;
            if (isxdigit(c)) r |= 0x80; if (isalpha(c)) r |= 0x100;
        } else if (t == 1 && c >= 0xc0 && c < 0x100) r = 0x100 | (c < 0xe0 ? 1 : 2);
        out[i] = r;
    }
    return TRUE;
}
static unsigned lat1_lower(unsigned c) { if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) return c + 32; return c; }
static unsigned lat1_upper(unsigned c) { if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) return c - 32; return c; }
static int WINAPI LCMapStringA_(DWORD lc, DWORD fl, const char *s, int n, char *d, int dn) {
    (void)lc;
    if (n < 0) n = (int)strlen(s) + 1;
    if (!dn) return n;
    int m = n < dn ? n : dn;
    for (int i = 0; i < m; i++) { unsigned c = (unsigned char)s[i]; d[i] = (char)((fl & 0x100) ? lat1_lower(c) : (fl & 0x200) ? lat1_upper(c) : c); }
    return m;
}
static int WINAPI LCMapStringW_(DWORD lc, DWORD fl, const WCHAR *s, int n, WCHAR *d, int dn) {
    (void)lc;
    if (n < 0) { n = 0; while (s[n]) n++; n++; }
    if (!dn) return n;
    int m = n < dn ? n : dn;
    for (int i = 0; i < m; i++) { unsigned c = s[i]; d[i] = (WCHAR)(c < 256 ? ((fl & 0x100) ? lat1_lower(c) : (fl & 0x200) ? lat1_upper(c) : c) : c); }
    return m;
}
static int WINAPI CompareStringA_(DWORD lc, DWORD fl, const char *a, int na, const char *b, int nb) {
    (void)lc;
    if (na < 0) na = (int)strlen(a);
    if (nb < 0) nb = (int)strlen(b);
    for (int i = 0; i < na && i < nb; i++) {
        unsigned x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (fl & 1) { x = lat1_lower(x); y = lat1_lower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return na == nb ? 2 : na < nb ? 1 : 3;
}
static int WINAPI CompareStringW_(DWORD lc, DWORD fl, const WCHAR *a, int na, const WCHAR *b, int nb) {
    (void)lc;
    if (na < 0) { na = 0; while (a[na]) na++; }
    if (nb < 0) { nb = 0; while (b[nb]) nb++; }
    for (int i = 0; i < na && i < nb; i++) {
        unsigned x = a[i], y = b[i];
        if (fl & 1) { x = lat1_lower(x); y = lat1_lower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return na == nb ? 2 : na < nb ? 1 : 3;
}
static int WINAPI lstrlenA_(const char *s) { return s ? (int)strlen(s) : 0; }
static char *WINAPI lstrcpyA_(char *d, const char *s) { if (!d || !s) return NULL; return strcpy(d, s); }
static char *WINAPI lstrcpynA_(char *d, const char *s, int n) { if (n <= 0) return d; strncpy(d, s, n - 1); d[n - 1] = 0; return d; }
static char *WINAPI lstrcatA_(char *d, const char *s) { if (!d || !s) return NULL; return strcat(d, s); }
static int WINAPI lstrcmpiA_(const char *a, const char *b) {
    for (;; a++, b++) {
        unsigned x = lat1_lower((unsigned char)*a), y = lat1_lower((unsigned char)*b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
static int WINAPI lstrcmpA_(const char *a, const char *b) { int r = strcmp(a, b); return r < 0 ? -1 : r > 0; }

/* ------------------------------------------------------------------ */
/* modules                                                              */
/* builtin DLLs get fake module handles so that GetProcAddress works */
static const char *g_fake_mods[] = { "kernel32", "user32", "gdi32", "advapi32", "ole32", "ddraw", "dinput", "dsound",
                                     "winmm", "wsock32", "winspool", "version", "dplayx", "binkw32", "ntdll", "msvcrt", "rpcrt4", "comctl32", "shell32", "imagehlp", "ws2_32", NULL };
#define FAKE_MOD_BASE 0x7fe00000u
static HMODULE fake_mod(const char *name) {
    char n[64]; size_t i = 0;
    const char *s = strrchr(name, '\\'); if (s) name = s + 1;
    for (; name[i] && i < 63; i++) n[i] = (char)tolower((unsigned char)name[i]);
    n[i] = 0;
    char *dot = strrchr(n, '.'); if (dot) *dot = 0;
    for (int k = 0; g_fake_mods[k]; k++) if (!strcmp(g_fake_mods[k], n)) return FAKE_MOD_BASE + k * 0x10000;
    return 0;
}
static HMODULE WINAPI GetModuleHandleA_(const char *name) {
    if (!name) return g_exe->base;
    module_t *m = pe_find_module(name);
    if (m) return m->base;
    HMODULE h = fake_mod(name);
    if (!h) set_last_error(126);
    return h;
}
extern int dll_has_builtins(const char *dn);
static HMODULE WINAPI LoadLibraryA_(const char *name) {
    LOG("LoadLibraryA(%s)", name);
    module_t *m = pe_find_module(name);
    if (m) return m->base;
    HMODULE h = fake_mod(name);
    if (h) return h;
    char hp[1024];
    if (win_host_path(name, hp, sizeof hp) && (m = pe_load(hp, name, 0))) return m->base;
    set_last_error(126);
    return 0;
}
static BOOL WINAPI FreeLibrary_(HMODULE h) { (void)h; return TRUE; }
static void *WINAPI GetProcAddress_(HMODULE h, const char *name) {
    int ord = ((uintptr_t)name < 0x10000) ? (int)(uintptr_t)name : 0;
    const char *nm = ord ? NULL : name;
    if (h >= FAKE_MOD_BASE && h < FAKE_MOD_BASE + 0x200000) {
        const char *dll = g_fake_mods[(h - FAKE_MOD_BASE) / 0x10000];
        void *p = rt_find_export(dll, nm, ord);
        LOG("GetProcAddress(%s, %s) -> %p", dll, nm ? nm : "#ord", p);
        if (!p) {
            char ob[16]; if (!nm) { snprintf(ob, sizeof ob, "#%d", ord); }
            /* GPxPatch modules are UPX-packed: their unpacker gives up on the first missing
               import. Hand them a thunk that reports the function if it is ever called. */
            uint32_t ra = (uint32_t)(uintptr_t)__builtin_return_address(0);
            module_t *cm = pe_module_by_addr(ra);
            if (cm && (strstr(cm->name, "gpx") || strstr(cm->name, ".gxm"))) {
                extern void *make_missing_thunk(const char *dll, const char *name);
                static const char *optional[] = { "AddFontResourceExA", "RemoveFontResourceExA", NULL };
                int opt = 0; for (int i = 0; nm && optional[i]; i++) if (!strcmp(nm, optional[i])) opt = 1;
                if (!opt) { LOG("  (stub for %s: reported if called)", nm ? nm : ob); return make_missing_thunk(dll, nm ? nm : ob); }
            }
        }
        if (!p) set_last_error(127);
        return p;
    }
    module_t *m = pe_module_by_base(h);
    if (m) return (void *)(uintptr_t)pe_get_proc(m, nm, ord);
    set_last_error(127);
    return NULL;
}
static DWORD WINAPI GetModuleFileNameA_(HMODULE h, char *buf, DWORD n) {
    char tmp[300];
    if (!h || h == g_exe->base) { extern char g_exe_file[256]; snprintf(tmp, sizeof tmp, "C:\\GP3\\%s", g_exe_file); }
    else {
        module_t *m = pe_module_by_base(h);
        if (m) snprintf(tmp, sizeof tmp, "C:\\GP3\\%s.dll", m->name);
        else snprintf(tmp, sizeof tmp, "C:\\WINDOWS\\SYSTEM32\\unknown.dll");
    }
    snprintf(buf, n, "%s", tmp);
    return (DWORD)strlen(buf);
}

/* ------------------------------------------------------------------ */
extern uint32_t WINAPI k32_RtlUnwind(void *, void *, void *, uint32_t);

#define E(n) { "kernel32", #n, 0, (void *)n##_ }
static const export_t k32_exports[] = {
    E(GetLastError), E(SetLastError), E(SetErrorMode), E(ExitProcess), E(TerminateProcess), E(GetCurrentProcess), E(CreateToolhelp32Snapshot), E(Process32First), E(Process32Next), E(OpenProcess), E(QueryFullProcessImageNameA),
    E(GetCurrentThread), E(GetCurrentThreadId), E(GetCurrentProcessId), E(GetVersion), E(GetVersionExA),
    E(GetCommandLineA), E(GetStartupInfoA), E(OutputDebugStringA), E(DebugBreak), E(SetUnhandledExceptionFilter),
    E(UnhandledExceptionFilter), E(RaiseException), E(GetSystemInfo), E(GlobalMemoryStatus), E(GetDriveTypeA),
    E(GetLogicalDrives), E(GetVolumeInformationA), E(GetSystemDirectoryA), E(GetWindowsDirectoryA), E(IsProcessorFeaturePresent),
    E(QueryPerformanceFrequency), E(QueryPerformanceCounter), E(Sleep), E(GetLocalTime), E(GetSystemTime),
    E(GetTimeZoneInformation), E(FileTimeToDosDateTime), E(VirtualAlloc), E(VirtualFree), E(VirtualProtect),
    E(HeapCreate), E(HeapDestroy), E(GetProcessHeap), E(HeapAlloc), E(HeapFree), E(HeapReAlloc), E(HeapSize),
    E(GlobalAlloc), E(GlobalFree), E(GlobalLock), E(GlobalUnlock), E(GlobalHandle), E(IsBadReadPtr), E(IsBadWritePtr),
    E(IsBadCodePtr), E(AddVectoredExceptionHandler), E(DisableThreadLibraryCalls), E(IsDebuggerPresent), E(VirtualQuery), E(GetWriteWatch), E(CreateWaitableTimerA), E(SetWaitableTimer), E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue), E(InitializeCriticalSection),
    E(DeleteCriticalSection), E(EnterCriticalSection), E(LeaveCriticalSection), E(TryEnterCriticalSection), E(GetProcessAffinityMask), E(SetProcessAffinityMask), E(OpenThread), E(SuspendThread), E(GetThreadContext), E(InterlockedIncrement),
    E(InterlockedDecrement), E(InterlockedExchange), E(CreateEventA), E(CreateSemaphoreA), E(CreateMutexA), E(SetEvent),
    E(ResetEvent), E(ReleaseSemaphore), E(ReleaseMutex), E(WaitForSingleObject), E(CreateThread), E(ResumeThread),
    E(ExitThread), E(GetExitCodeThread), E(SetThreadPriority), E(GetThreadPriority), E(CloseHandle), E(GetStdHandle),
    E(SetStdHandle), E(GetFileType), E(SetHandleCount), E(CreateFileA), E(ReadFile), E(WriteFile), E(SetFilePointer),
    E(GetFileSize), E(SetEndOfFile), E(FlushFileBuffers), E(GetFileTime), E(GetFileAttributesA), E(SetFileAttributesA),
    E(CreateDirectoryA), E(DeleteFileA), E(MoveFileA), E(GetCurrentDirectoryA), E(SetCurrentDirectoryA),
    E(FindFirstFileA), E(FindNextFileA), E(FindClose), E(GetPrivateProfileStringA), E(GetPrivateProfileIntA),
    E(GetEnvironmentStrings), E(GetEnvironmentStringsW), E(GetEnvironmentVariableA), E(SetEnvironmentVariableA),
    E(MultiByteToWideChar), E(WideCharToMultiByte), E(GetACP), E(GetOEMCP), E(GetCPInfo), E(GetStringTypeA),
    E(GetStringTypeW), E(LCMapStringA), E(LCMapStringW), E(CompareStringA), E(CompareStringW), E(lstrlenA),
    E(lstrcpyA), E(lstrcpynA), E(lstrcatA), E(lstrcmpiA), E(lstrcmpA), E(GetModuleHandleA), E(LoadLibraryA),
    E(FreeLibrary), E(GetProcAddress), E(GetModuleFileNameA),
    { "kernel32", "FreeEnvironmentStringsA", 0, (void *)FreeEnvironmentStrings_ },
    { "kernel32", "FreeEnvironmentStringsW", 0, (void *)FreeEnvironmentStrings_ },
    { "kernel32", "RtlUnwind", 0, (void *)k32_RtlUnwind },
    { "kernel32", "GetTickCount", 0, (void *)k32_GetTickCount },
    { NULL, NULL, 0, NULL }
};
#undef E

void kernel32_register(void) { rt_register_exports(k32_exports); }
