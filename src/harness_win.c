/* Extra Win32 surface needed to run the *Windows* build of gp3rt ddraw.dll
 * inside the Linux runner (test harness): msvcrt.dll, ntdll helpers, opengl32
 * stubs and a few kernel32/user32/gdi32 functions. */
#include "rt.h"
#include "plat.h"
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

extern int win_host_path(const char *win, char *host, size_t n);

/* ------------------------------------------------------------------ */
/* msvcrt                                                               */
static FILE *CDECL m_fopen(const char *p, const char *mode) {
    char hp[1024];
    win_host_path(p, hp, sizeof hp);
    FILE *f = fopen(hp, mode);
    LOG("msvcrt fopen(%s, %s) -> %s %s", p, mode, hp, f ? "ok" : "failed");
    /* on Windows fopen goes through CreateFile, which sets the Win32 last error: GPx's image
       loader turns a missing file into an error from GetLastError() (0 would mean success) */
    if (!f) { extern void set_last_error_ext(DWORD); set_last_error_ext(errno == ENOENT ? 2 /* ERROR_FILE_NOT_FOUND */ : errno == ENOTDIR ? 3 : errno == EACCES ? 5 : 2); }
    return f;
}
static int CDECL m_vsnprintf(char *b, size_t n, const char *f, va_list ap) {
    int r = vsnprintf(b, n, f, ap);
    return (r < 0 || (size_t)r >= n) ? -1 : r;   /* msvcrt semantics */
}
static uint8_t g_fake_iob[3 * 32];
static void *CDECL m_iob_func(void) { return g_fake_iob; }
static int CDECL m_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
static int CDECL m_strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static void CDECL m_exit(int c) { LOG("msvcrt exit(%d)", c); _exit(c); }
/* cdecl wrappers so the calling convention is explicit */
#define W1(r, n, t1) static r CDECL m_##n(t1 a) { return n(a); }
#define W2(r, n, t1, t2) static r CDECL m_##n(t1 a, t2 b) { return n(a, b); }
#define W3(r, n, t1, t2, t3) static r CDECL m_##n(t1 a, t2 b, t3 c) { return n(a, b, c); }
#define W4(r, n, t1, t2, t3, t4) static r CDECL m_##n(t1 a, t2 b, t3 c, t4 d) { return n(a, b, c, d); }
W1(void *, malloc, size_t) W2(void *, calloc, size_t, size_t) W2(void *, realloc, void *, size_t)
static void CDECL m_free(void *p) { free(p); }
W3(void *, memcpy, void *, const void *, size_t) W3(void *, memmove, void *, const void *, size_t)
W3(void *, memset, void *, int, size_t) W3(int, memcmp, const void *, const void *, size_t)
W1(size_t, strlen, const char *) W2(int, strcmp, const char *, const char *) W3(int, strncmp, const char *, const char *, size_t)
W2(char *, strcpy, char *, const char *) W3(char *, strncpy, char *, const char *, size_t) W2(char *, strcat, char *, const char *)
W2(char *, strchr, const char *, int) W2(char *, strrchr, const char *, int) W2(char *, strstr, const char *, const char *)
static int CDECL m_fclose(FILE *f) { return f ? fclose(f) : -1; }
static size_t CDECL m_fread(void *b, size_t s, size_t n, FILE *f) { return f ? fread(b, s, n, f) : 0; }
static size_t CDECL m_fwrite(const void *b, size_t s, size_t n, FILE *f) { return f ? fwrite(b, s, n, f) : 0; }
W1(int, fflush, FILE *) W2(int, fputc, int, FILE *) W2(int, fputs, const char *, FILE *) W3(int, vfprintf, FILE *, const char *, va_list)
static int CDECL m_fseek(FILE *f, long o, int w) { return f ? fseek(f, o, w) : -1; }
static long CDECL m_ftell(FILE *f) { return f ? ftell(f) : -1; }
static char *CDECL m_fgets(char *b, int n, FILE *f) { return f ? fgets(b, n, f) : NULL; }
W1(int, atoi, const char *) W1(double, atof, const char *) W3(unsigned long, strtoul, const char *, char **, int) W3(long, strtol, const char *, char **, int)
W1(char *, getenv, const char *) W1(int, abs, int)
W1(double, floor, double) W1(double, ceil, double) W1(double, exp, double) W1(double, log, double) W2(double, pow, double, double)
W1(double, sqrt, double) W1(double, fabs, double)
static int CDECL m_sprintf(char *b, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsprintf(b, f, ap); va_end(ap); return r; }
static int CDECL m_fprintf(FILE *fp, const char *f, ...) { va_list ap; va_start(ap, f); int r = vfprintf(fp, f, ap); va_end(ap); return r; }
static int CDECL m_sscanf(const char *s, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsscanf(s, f, ap); va_end(ap); return r; }

/* _chkstk: eax = bytes to allocate below the caller's stack pointer */
__attribute__((naked, unused)) static void m_chkstk(void) {
    __asm__ volatile(
        "push %ecx\n"
        "lea 8(%esp), %ecx\n"
        "sub %eax, %ecx\n"          /* ecx = new stack pointer */
        "mov %esp, %eax\n"          /* eax -> [saved ecx][return address] */
        "mov %ecx, %esp\n"
        "mov (%eax), %ecx\n"
        "push 4(%eax)\n"
        "ret\n");
}

/* ntdll 64-bit helpers (stdcall, 16 bytes of arguments) */
static int64_t WINAPI n_alldiv(int64_t a, int64_t b) { return a / b; }
static uint64_t WINAPI n_aulldiv(uint64_t a, uint64_t b) { return a / b; }
static uint64_t WINAPI n_aullrem(uint64_t a, uint64_t b) { return a % b; }
static int64_t WINAPI n_allrem(int64_t a, int64_t b) { return a % b; }
static int64_t WINAPI n_allmul(int64_t a, int64_t b) { return a * b; }

/* opengl32: not available in the harness (the DLL then uses its software renderer) */
static uint32_t WINAPI o_zero1(uint32_t a) { (void)a; return 0; }
static uint32_t WINAPI o_zero2(uint32_t a, uint32_t b) { (void)a; (void)b; return 0; }

#define M(n) { "msvcrt", #n, 0, (void *)m_##n }
static const export_t harness_exports[] = {
    M(malloc), M(calloc), M(realloc), M(free), M(memcpy), M(memmove), M(memset), M(memcmp), M(strlen), M(strcmp), M(strncmp),
    M(strcpy), M(strncpy), M(strcat), M(strchr), M(strrchr), M(strstr), M(fopen), M(fclose), M(fread), M(fwrite), M(fflush),
    M(fputc), M(fputs), M(fprintf), M(vfprintf), M(fseek), M(ftell), M(fgets), M(atoi), M(atof), M(strtoul), M(strtol),
    M(getenv), M(exit), M(abs), M(floor), M(ceil), M(exp), M(log), M(pow), M(sqrt), M(fabs), M(sprintf), M(sscanf),
    { "msvcrt", "_stricmp", 0, (void *)m_stricmp }, { "msvcrt", "_strnicmp", 0, (void *)m_strnicmp },
    { "msvcrt", "_vsnprintf", 0, (void *)m_vsnprintf }, { "msvcrt", "__iob_func", 0, (void *)m_iob_func },
    { "msvcrt", "_exit", 0, (void *)m_exit },
    { "ntdll", "_alldiv", 0, (void *)n_alldiv }, { "ntdll", "_aulldiv", 0, (void *)n_aulldiv }, { "ntdll", "_aullrem", 0, (void *)n_aullrem },
    { "ntdll", "_allrem", 0, (void *)n_allrem }, { "ntdll", "_allmul", 0, (void *)n_allmul },
    { "opengl32", "wglCreateContext", 0, (void *)o_zero1 }, { "opengl32", "wglDeleteContext", 0, (void *)o_zero1 },
    { "opengl32", "wglGetProcAddress", 0, (void *)o_zero1 }, { "opengl32", "wglMakeCurrent", 0, (void *)o_zero2 },
    { NULL, NULL, 0, NULL }
};
#undef M
void harness_register(void) { rt_register_exports(harness_exports); }
