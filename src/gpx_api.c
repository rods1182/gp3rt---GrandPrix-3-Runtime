/* Extra Win32 / MSVCRT surface used by GPxPatch's modules (GPxPSup.dll, *.gxm,
 * GPxPDX7.dll) so the Linux runtime can host them the way GPxPatch does on Windows. */
#include "rt.h"
#include "plat.h"
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

extern int win_host_path(const char *win, char *host, size_t n);

/* ------------------------------------------------------------------ */
/* kernel32: named shared memory (GPxDataShare), local heap, misc        */
typedef struct mapping { char name[128]; uint8_t *mem; uint32_t size; struct mapping *next; } mapping_t;
static mapping_t *g_maps;
static void map_close(hobj_t *o) { (void)o; /* kept alive: views may outlive the handle */ }
static HANDLE WINAPI CreateFileMappingA_(HANDLE f, void *sa, DWORD prot, DWORD hi, DWORD lo, const char *name) {
    (void)sa; (void)prot; (void)hi;
    if (name) for (mapping_t *m = g_maps; m; m = m->next) if (!strcasecmp(m->name, name)) {
        set_last_error(183 /* ERROR_ALREADY_EXISTS */); return handle_new(H_MAPPING, m, map_close);
    }
    mapping_t *m = calloc(1, sizeof *m);
    if (name) snprintf(m->name, sizeof m->name, "%s", name);
    m->size = lo ? lo : 4096;
    m->mem = calloc(1, m->size);
    if (f && f != (HANDLE)(intptr_t)-1) LOG("CreateFileMappingA on a file handle (%s): anonymous memory used", name ? name : "-");
    m->next = g_maps; g_maps = m;
    set_last_error(0);
    LOG("CreateFileMappingA(%s, %u bytes)", name ? name : "-", m->size);
    return handle_new(H_MAPPING, m, map_close);
}
static HANDLE WINAPI OpenFileMappingA_(DWORD acc, BOOL inh, const char *name) {
    (void)acc; (void)inh;
    for (mapping_t *m = g_maps; m && name; m = m->next) if (!strcasecmp(m->name, name)) return handle_new(H_MAPPING, m, map_close);
    set_last_error(2); return 0;
}
static void *WINAPI MapViewOfFile_(HANDLE h, DWORD acc, DWORD hi, DWORD lo, DWORD n) {
    (void)acc; (void)hi; (void)n;
    hobj_t *o = handle_get(h, H_MAPPING); if (!o) return NULL;
    mapping_t *m = o->p; return m->mem + lo;
}
static BOOL WINAPI UnmapViewOfFile_(void *p) { (void)p; return TRUE; }
static void *WINAPI LocalAlloc_(UINT fl, DWORD n) { (void)fl; return calloc(1, n ? n : 1); }
static void *WINAPI LocalFree_(void *p) { free(p); return NULL; }
static void *WINAPI LocalLock_(void *p) { return p; }
static BOOL WINAPI LocalUnlock_(void *p) { (void)p; return TRUE; }
static int WINAPI MulDiv_(int a, int b, int c) { if (!c) return -1; int64_t r = (int64_t)a * b; return (int)((r + (((r < 0) ^ (c < 0)) ? -c / 2 : c / 2)) / c); }
static BOOL WINAPI FlushInstructionCache_(HANDLE p, const void *a, size_t n) { (void)p; (void)a; (void)n; return TRUE; }
static BOOL WINAPI VirtualProtectEx_(HANDLE p, void *a, DWORD n, DWORD np, DWORD *old) {
    (void)p; typedef BOOL (WINAPI *vp_t)(void *, DWORD, DWORD, DWORD *);
    vp_t vp = (vp_t)rt_find_export("kernel32", "VirtualProtect", 0); return vp ? vp(a, n, np, old) : FALSE;
}
static size_t WINAPI VirtualQueryEx_(HANDLE p, const void *a, uint32_t *mbi, size_t n) {
    (void)p; typedef size_t (WINAPI *vq_t)(const void *, uint32_t *, size_t);
    vq_t vq = (vq_t)rt_find_export("kernel32", "VirtualQuery", 0); return vq ? vq(a, mbi, n) : 0;
}
static DWORD WINAPI GetFileAttributesEx_unused(void) { return 0; }
static DWORD WINAPI FormatMessageA_(DWORD fl, const void *src, DWORD id, DWORD lang, char *buf, DWORD n, void *args) {
    (void)src; (void)lang; (void)args;
    char tmp[64]; snprintf(tmp, sizeof tmp, "error %u", (unsigned)id);
    if (fl & 0x100 /* ALLOCATE_BUFFER */) { char *p = strdup(tmp); *(char **)buf = p; return (DWORD)strlen(p); }
    if (buf && n) { snprintf(buf, n, "%s", tmp); return (DWORD)strlen(buf); }
    return 0;
}
static BOOL WINAPI ReadProcessMemory_(HANDLE p, const void *a, void *b, DWORD n, DWORD *rd) { (void)p; memcpy(b, a, n); if (rd) *rd = n; return TRUE; }
static BOOL WINAPI WriteProcessMemory_(HANDLE p, void *a, const void *b, DWORD n, DWORD *wr) { (void)p;
    { static int k; if (getenv("GP3_GPX_TRACE") && k++ < 2000) LOG("WriteProcessMemory(%p, %u) from %08x", a, n, (unsigned)(uintptr_t)__builtin_return_address(0)); }
    memcpy(a, b, n); if (wr) *wr = n; return TRUE; }
static uint32_t WINAPI GetModuleHandleW_(const uint16_t *n) {
    char a[260]; int i = 0; if (n) for (; n[i] && i < 259; i++) a[i] = (char)n[i]; a[i] = 0;
    typedef uint32_t (WINAPI *gmh_t)(const char *); gmh_t g = (gmh_t)rt_find_export("kernel32", "GetModuleHandleA", 0); return g ? g(n ? a : NULL) : 0;
}
static uint32_t WINAPI RtlGetVersion_(uint32_t *vi) { vi[1] = 5; vi[2] = 1; vi[3] = 2600; vi[4] = 2; return 0; }   /* reported as Windows XP, like the game sees */
static BOOL WINAPI CopyFileA_(const char *a, const char *b, BOOL fail) {
    char h1[1024], h2[1024]; win_host_path(a, h1, sizeof h1); win_host_path(b, h2, sizeof h2);
    if (fail && access(h2, F_OK) == 0) return FALSE;
    FILE *i = fopen(h1, "rb"); if (!i) return FALSE; FILE *o = fopen(h2, "wb"); if (!o) { fclose(i); return FALSE; }
    char buf[65536]; size_t r; while ((r = fread(buf, 1, sizeof buf, i)) > 0) fwrite(buf, 1, r, o); fclose(i); fclose(o); return TRUE;
}
static uint32_t WINAPI UuidCreate_(uint8_t *u) { for (int i = 0; i < 16; i++) u[i] = (uint8_t)rand(); u[7] = (u[7] & 0x0f) | 0x40; return 0; }
/* wsock32 (GPxLap's online lap records / network bits): report no network */
static int WINAPI WSAStartup_(WORD v, uint8_t *d) { memset(d, 0, 400); *(WORD *)d = v; *(WORD *)(d + 2) = 0x0202; return 0; }
static int WINAPI WSACleanup_(void) { return 0; }
static int WINAPI WSAGetLastError_(void) { return 10050; /* WSAENETDOWN */ }
static void *WINAPI gethostbyname_(const char *n) { (void)n; return NULL; }
static int WINAPI gethostname_(char *b, int n) { snprintf(b, n, "gp3"); return 0; }
static uint32_t WINAPI socket_(int a, int t, int p) { (void)a; (void)t; (void)p; return (uint32_t)-1; }
static int WINAPI closesocket_(uint32_t s) { (void)s; return 0; }
static uint32_t WINAPI inet_addr_(const char *s) { (void)s; return 0xffffffff; }
static uint16_t WINAPI htons_(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint32_t WINAPI htonl_(uint32_t v) { return __builtin_bswap32(v); }
static BOOL WINAPI SetThreadContext_(HANDLE t, const void *c) { (void)t; (void)c; LOG("SetThreadContext (ignored)"); return TRUE; }

/* INI files: sections and writes (GPxPatch keeps all its settings in .ini files) */
static FILE *ini_open(const char *file, const char *mode, char *hp, size_t n) {
    char fp[1024];
    if (file && !strchr(file, '\\') && !strchr(file, '/')) snprintf(fp, sizeof fp, "C:\\WINDOWS\\%s", file);
    else snprintf(fp, sizeof fp, "%s", file ? file : "");
    win_host_path(fp, hp, n);
    return fopen(hp, mode);
}
static DWORD WINAPI GetPrivateProfileSectionA_(const char *app, char *buf, DWORD n, const char *file) {
    char hp[1024]; FILE *f = ini_open(file, "r", hp, sizeof hp);
    TRACE("GetPrivateProfileSectionA([%s], %s) %s", app, file, f ? "" : "(no file)");
    if (getenv("GP3_INI_TRACE")) LOG("INISECT %s|%s", file ? file : "", app ? app : "");
    DWORD o = 0; char line[1024]; int in = 0;
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *s = line; while (isspace((unsigned char)*s)) s++;
            char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
            if (*s == '[') { char *r = strchr(s, ']'); if (r) *r = 0; in = !strcasecmp(s + 1, app); continue; }
            if (!in || !*s || *s == ';') continue;
            size_t l = strlen(s); if (o + l + 2 > n) break;
            memcpy(buf + o, s, l + 1); o += (DWORD)l + 1;
        }
        fclose(f);
    }
    if (n) { if (o + 1 > n) o = n - 1; buf[o] = 0; }
    return o;
}
static DWORD WINAPI GetPrivateProfileSectionNamesA_(char *buf, DWORD n, const char *file) {
    char hp[1024]; FILE *f = ini_open(file, "r", hp, sizeof hp);
    TRACE("GetPrivateProfileSectionNamesA(%s) %s", file, f ? "" : "(no file)");
    DWORD o = 0; char line[1024];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *s = line; while (isspace((unsigned char)*s)) s++;
            if (*s != '[') continue;
            char *r = strchr(s, ']'); if (!r) continue; *r = 0;
            size_t l = strlen(s + 1); if (o + l + 2 > n) break;
            memcpy(buf + o, s + 1, l + 1); o += (DWORD)l + 1;
        }
        fclose(f);
    }
    if (n) buf[o < n ? o : n - 1] = 0;
    return o;
}
static BOOL WINAPI WritePrivateProfileStringA_(const char *app, const char *key, const char *val, const char *file) {
    if (!app) return TRUE;
    char hp[1024]; FILE *f = ini_open(file, "r", hp, sizeof hp);
    size_t cap = 1 << 16, len = 0; char *txt = malloc(cap); txt[0] = 0;
    if (f) { size_t r; while ((r = fread(txt + len, 1, cap - len - 1, f)) > 0) { len += r; if (len + 1 >= cap) { cap *= 2; txt = realloc(txt, cap); } } fclose(f); txt[len] = 0; }
    /* rebuild line by line */
    size_t ocap = len + 1024, ol = 0; char *out = malloc(ocap);
    #define PUT(s) do { size_t _l = strlen(s); if (ol + _l + 1 >= ocap) { ocap = (ol + _l) * 2 + 64; out = realloc(out, ocap); } memcpy(out + ol, s, _l); ol += _l; out[ol] = 0; } while (0)
    int in = 0, done = 0, seen = 0; char *p = txt;
    while (*p) {
        char *nl = strchr(p, '\n'); size_t l = nl ? (size_t)(nl - p + 1) : strlen(p);
        char line[1024]; size_t ll = l < sizeof line - 1 ? l : sizeof line - 1; memcpy(line, p, ll); line[ll] = 0; p += l;
        char *s = line; while (isspace((unsigned char)*s)) s++;
        if (*s == '[') {
            if (in && !done && key && val) { char b[1100]; snprintf(b, sizeof b, "%s=%s\r\n", key, val); PUT(b); done = 1; }
            char nm[512]; snprintf(nm, sizeof nm, "%s", s + 1); char *r = strchr(nm, ']'); if (r) *r = 0;
            in = !strcasecmp(nm, app); if (in) seen = 1;
            if (in && !key) continue;   /* delete section */
            PUT(line); continue;
        }
        if (in) {
            if (!key) continue;
            char *eq = strchr(s, '=');
            if (eq) { char k[512]; size_t kl = (size_t)(eq - s); if (kl > 511) kl = 511; memcpy(k, s, kl); k[kl] = 0; while (kl && isspace((unsigned char)k[kl - 1])) k[--kl] = 0;
                if (!strcasecmp(k, key)) { if (val && !done) { char b[1100]; snprintf(b, sizeof b, "%s=%s\r\n", key, val); PUT(b); done = 1; } continue; } }
        }
        PUT(line);
    }
    if (key && val && !done) {
        char b[1100];
        if (!seen) { if (ol && out[ol - 1] != '\n') PUT("\r\n"); snprintf(b, sizeof b, "[%s]\r\n", app); PUT(b); }
        snprintf(b, sizeof b, "%s=%s\r\n", key, val); PUT(b);
    }
    #undef PUT
    f = fopen(hp, "wb"); if (f) { fwrite(out, 1, ol, f); fclose(f); }
    free(txt); free(out);
    return f != NULL;
}
static BOOL WINAPI WritePrivateProfileSectionA_(const char *app, const char *data, const char *file) {
    WritePrivateProfileStringA_(app, NULL, NULL, file);
    for (const char *s = data; s && *s; s += strlen(s) + 1) {
        char k[512]; const char *eq = strchr(s, '='); if (!eq) continue;
        size_t kl = (size_t)(eq - s); if (kl > 511) kl = 511; memcpy(k, s, kl); k[kl] = 0;
        WritePrivateProfileStringA_(app, k, eq + 1, file);
    }
    return TRUE;
}
static DWORD WINAPI GetFullPathNameA_(const char *n, DWORD sz, char *b, char **fp) {
    char cur[512] = "C:\\GP3";
    if (n[0] && n[1] == ':') snprintf(b, sz, "%s", n); else snprintf(b, sz, "%s\\%s", cur, n);
    if (fp) { char *s = strrchr(b, '\\'); *fp = s ? s + 1 : b; }
    return (DWORD)strlen(b);
}

/* ------------------------------------------------------------------ */
/* msvcrt (cdecl)                                                       */
static uint8_t g_iob[3 * 32];
static FILE *fpm(FILE *f) {
    uint8_t *p = (uint8_t *)f;
    if (p >= g_iob && p < g_iob + sizeof g_iob) return (p - g_iob) / 32 == 0 ? stdin : (p - g_iob) / 32 == 1 ? stdout : stderr;
    return f;
}
static int CDECL c_fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(fpm(f), fmt, ap); va_end(ap); return r; }
static int CDECL c_vfprintf(FILE *f, const char *fmt, va_list ap) { return vfprintf(fpm(f), fmt, ap); }
static int CDECL c_fputs(const char *s, FILE *f) { return fputs(s, fpm(f)); }
static int CDECL c_fputc(int c, FILE *f) { return fputc(c, fpm(f)); }
static int CDECL c_fflush(FILE *f) { return f ? fflush(fpm(f)) : 0; }
static int CDECL c_printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }
static int CDECL c_vsprintf(char *b, const char *fmt, va_list ap) { return vsprintf(b, fmt, ap); }
static int CDECL c_snprintf(char *b, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(b, n, fmt, ap); va_end(ap); return (r < 0 || (size_t)r >= n) ? -1 : r; }
static void *CDECL c_new(size_t n) { return calloc(1, n ? n : 1); }
static void CDECL c_delete(void *p) { free(p); }
static void CDECL c_purecall(void) { LOG("msvcrt: pure virtual call"); }
typedef void (CDECL *vfn_t)(void);
static void CDECL c_initterm(vfn_t *a, vfn_t *b) { for (; a < b; a++) if (*a) (*a)(); }
static void *CDECL c_onexit(void *f) { return f; }
static void *CDECL c_dllonexit(void *f, void *b, void *e) { (void)b; (void)e; return f; }
static int g_adjust_fdiv = 0;
static int CDECL c_finite(double d) { return isfinite(d); }
static int CDECL c_isnan(double d) { return isnan(d); }
static char *CDECL c_strdup(const char *s) { return strdup(s); }
static int CDECL c_iscntrl(int c) { return iscntrl(c); }
static int CDECL c_isalpha(int c) { return isalpha(c); } static int CDECL c_isdigit(int c) { return isdigit(c); }
static int CDECL c_isspace(int c) { return isspace(c); } static int CDECL c_isalnum(int c) { return isalnum(c); }
static int CDECL c_isupper(int c) { return isupper(c); } static int CDECL c_islower(int c) { return islower(c); }
static int CDECL c_isprint(int c) { return isprint(c); } static int CDECL c_isxdigit(int c) { return isxdigit(c); }
static int CDECL c_tolower(int c) { return tolower(c); } static int CDECL c_toupper(int c) { return toupper(c); }
static int32_t CDECL c_time(int32_t *t) { int32_t v = (int32_t)time(NULL); if (t) *t = v; return v; }
/* struct tm has the same 9 leading ints on both sides */
static struct tm *CDECL c_localtime(const int32_t *t) { time_t v = *t; static struct tm r; localtime_r(&v, &r); return &r; }
static struct tm *CDECL c_gmtime(const int32_t *t) { time_t v = *t; static struct tm r; gmtime_r(&v, &r); return &r; }
static size_t CDECL c_strftime(char *b, size_t n, const char *f, const struct tm *t) { return strftime(b, n, f, t); }
static char *CDECL c_setlocale(int c, const char *l) { (void)c; (void)l; return "C"; }
static char *CDECL c_strerror(int e) { return strerror(e); }
static double CDECL c_strtod(const char *s, char **e) { return strtod(s, e); }
static char *CDECL c_strtok(char *s, const char *d) { return strtok(s, d); }
static int g_errno_v;
static int *CDECL c_errno(void) { g_errno_v = errno; return &g_errno_v; }
static int CDECL c_access(const char *p, int m) { char hp[1024]; win_host_path(p, hp, sizeof hp); return access(hp, m & 6 ? (m & 2 ? W_OK : R_OK) : F_OK); }
static char *CDECL c_gcvt(double v, int d, char *b) { sprintf(b, "%.*g", d, v); return b; }
static char *CDECL c_ltoa(long v, char *b, int r) { if (r == 10) sprintf(b, "%ld", v); else if (r == 16) sprintf(b, "%lx", (unsigned long)v); else { unsigned long u = (unsigned long)v; char t[40]; int i = 0; do { int d = u % r; t[i++] = d < 10 ? '0' + d : 'a' + d - 10; u /= r; } while (u); int k = 0; while (i) b[k++] = t[--i]; b[k] = 0; } return b; }
static char *CDECL c_ultoa(unsigned long v, char *b, int r) { char t[40]; int i = 0; do { int d = v % r; t[i++] = d < 10 ? '0' + d : 'a' + d - 10; v /= r; } while (v); int k = 0; while (i) b[k++] = t[--i]; b[k] = 0; return b; }
static char *CDECL c_itoa(int v, char *b, int r) { return c_ltoa(v, b, r); }
static void CDECL c_splitpath(const char *p, char *drv, char *dir, char *fn, char *ext) {
    const char *s = p;
    if (drv) { if (p[0] && p[1] == ':') { drv[0] = p[0]; drv[1] = ':'; drv[2] = 0; } else drv[0] = 0; }
    if (p[0] && p[1] == ':') s = p + 2;
    const char *ls = NULL; for (const char *q = s; *q; q++) if (*q == '\\' || *q == '/') ls = q;
    const char *base = ls ? ls + 1 : s;
    if (dir) { size_t l = ls ? (size_t)(ls - s + 1) : 0; memcpy(dir, s, l); dir[l] = 0; }
    const char *dot = strrchr(base, '.');
    if (fn) { size_t l = dot ? (size_t)(dot - base) : strlen(base); memcpy(fn, base, l); fn[l] = 0; }
    if (ext) { if (dot) strcpy(ext, dot); else ext[0] = 0; }
}
static void CDECL c_makepath(char *p, const char *drv, const char *dir, const char *fn, const char *ext) {
    p[0] = 0;
    if (drv && *drv) { strcat(p, drv); if (drv[1] != ':') strcat(p, ":"); }
    if (dir && *dir) { strcat(p, dir); size_t l = strlen(p); if (p[l - 1] != '\\' && p[l - 1] != '/') strcat(p, "\\"); }
    if (fn) strcat(p, fn);
    if (ext && *ext) { if (*ext != '.') strcat(p, "."); strcat(p, ext); }
}
static char *CDECL c_strlwr(char *s) { for (char *p = s; *p; p++) *p = tolower((unsigned char)*p); return s; }
static char *CDECL c_strupr(char *s) { for (char *p = s; *p; p++) *p = toupper((unsigned char)*p); return s; }
static int CDECL c_rand(void) { return rand() & 0x7fff; }
static void CDECL c_srand(unsigned s) { srand(s); }
static void CDECL c_qsort(void *b, size_t n, size_t w, int (CDECL *cmp)(const void *, const void *)) { qsort(b, n, w, (int (*)(const void *, const void *))cmp); }
static double CDECL c_sin(double x) { return sin(x); } static double CDECL c_cos(double x) { return cos(x); }
static double CDECL c_atan2(double y, double x) { return atan2(y, x); } static double CDECL c_fmod(double a, double b) { return fmod(a, b); }
static long CDECL c_labs(long v) { return labs(v); }
static int CDECL c_remove(const char *p) { char hp[1024]; win_host_path(p, hp, sizeof hp); return remove(hp); }
static int CDECL c_rename(const char *a, const char *b) { char h1[1024], h2[1024]; win_host_path(a, h1, sizeof h1); win_host_path(b, h2, sizeof h2); return rename(h1, h2); }
static int CDECL c_mkdir(const char *p) { char hp[1024]; win_host_path(p, hp, sizeof hp); return mkdir(hp, 0755); }
static int CDECL c_stat(const char *p, uint32_t *st) {
    char hp[1024]; win_host_path(p, hp, sizeof hp); struct stat s; if (stat(hp, &s)) return -1;
    memset(st, 0, 36); ((uint16_t *)st)[3] = S_ISDIR(s.st_mode) ? 0x4000 : 0x8000; st[5] = (uint32_t)s.st_size; st[6] = st[7] = st[8] = (uint32_t)s.st_mtime; return 0;
}
static int CDECL c_ungetc(int c, FILE *f) { return ungetc(c, fpm(f)); }
static int CDECL c_fgetc(FILE *f) { return fgetc(fpm(f)); }
static int CDECL c_feof(FILE *f) { return feof(fpm(f)); }
static int CDECL c_ferror(FILE *f) { return ferror(fpm(f)); }
static void CDECL c_rewind(FILE *f) { rewind(fpm(f)); }
static int CDECL c_fscanf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfscanf(fpm(f), fmt, ap); va_end(ap); return r; }
static int CDECL c_puts(const char *s) { return puts(s); }
static char *CDECL c_strpbrk(const char *s, const char *a) { return strpbrk(s, a); }
static size_t CDECL c_strspn(const char *s, const char *a) { return strspn(s, a); }
static size_t CDECL c_strcspn(const char *s, const char *a) { return strcspn(s, a); }
static char *CDECL c_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static void *CDECL c_memchr(const void *s, int c, size_t n) { return memchr(s, c, n); }
static int CDECL c_atexit(void *f) { (void)f; return 0; }
static void CDECL c_abort(void) { rt_fatal("msvcrt abort()"); }
/* MSVC C++ throw: ThrowInfo { attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray },
   CatchableTypeArray { n, CatchableType *[n] }, CatchableType { properties, TypeDescriptor *,
   mdisp, pdisp, vdisp, size, copyFunction }, TypeDescriptor { vtable, spare, name[] } */
static const char *cxx_type_name(const uint32_t *ti) {
    if (!ti || !ti[3]) return "?";
    const uint32_t *cta = (const uint32_t *)(uintptr_t)ti[3];
    if (!cta[0] || !cta[1]) return "?";
    const uint32_t *ct = (const uint32_t *)(uintptr_t)cta[1];
    return ct[1] ? (const char *)(uintptr_t)(ct[1] + 8) : "?";
}
void seh_raise_sw(uint32_t code, uint32_t nparams, const uint32_t *params, uint32_t addr);
static void CDECL c_CxxThrowException(void *obj, void *info) {
    static int n;
    if (n++ < 20) LOG("GPx: C++ exception %s thrown by a module", cxx_type_name(info));
    uint32_t p[3] = { 0x19930520u, (uint32_t)(uintptr_t)obj, (uint32_t)(uintptr_t)info };
    seh_raise_sw(0xe06d7363u, 3, p, (uint32_t)(uintptr_t)__builtin_return_address(0));
    LOG("GPx: nobody caught the C++ exception %s", cxx_type_name(info));
    rt_fatal("C++ exception in a GPxPatch module was not caught");
}
/* --- structured exception handling used by the GPx modules (MSVC 6 runtime) ---------------
   GPxPatch wraps its calls into the other modules in try { } catch (...) { }, which with
   Visual C++ 6 also catches access violations: a faulty display (e.g. a GPxSat page drawn
   before the timing data exists) is simply skipped for that frame instead of ending the
   game. These are the two frame handlers of that runtime: __CxxFrameHandler (catch (...)
   only; C++ throw is not supported) and _except_handler3 (__try / __except / __finally). */
uint32_t eh_call_ebp(uint32_t fn, uint32_t ebp);          /* calls a funclet with the frame's ebp */
void eh_resume(uint32_t esp, uint32_t ebp, uint32_t eip) __attribute__((noreturn));
int c_CxxFrameHandler(void *rec, void *frame, void *ctx, void *disp);
__asm__(".text\n"
    ".globl eh_call_ebp\neh_call_ebp:\n"
    "  push %ebp\n  push %ebx\n  push %esi\n  push %edi\n"
    "  mov 0x14(%esp), %ecx\n  mov 0x18(%esp), %ebp\n  call *%ecx\n"
    "  pop %edi\n  pop %esi\n  pop %ebx\n  pop %ebp\n  ret\n"
    ".globl eh_resume\neh_resume:\n"
    "  mov 0xc(%esp), %ecx\n  mov 0x8(%esp), %ebp\n  mov 0x4(%esp), %esp\n  jmp *%ecx\n"
    ".globl c_CxxFrameHandler\nc_CxxFrameHandler:\n"          /* eax = FuncInfo */
    "  push %eax\n  pushl 0x14(%esp)\n  pushl 0x14(%esp)\n  pushl 0x14(%esp)\n  pushl 0x14(%esp)\n"
    "  call cxx_frame_handler\n  add $0x14, %esp\n  ret\n");
static void set_except_list(uint32_t f) { __asm__ volatile("movl %0, %%fs:0" : : "r"(f) : "memory"); }
static int g_eh_logged;
static void eh_log(const char *kind, const uint32_t *er) {
    if (g_eh_logged++ < 20) LOG("GPx: %s caught exception %08x at %08x (the module skips that step, as on Windows)", kind, er[0], er[3]);
}
int CDECL cxx_frame_handler(uint32_t *er, uint32_t frame, void *ctx, void *disp, uint32_t *fi) {
    (void)ctx; (void)disp;
    if ((fi[0] & 0xfffffff0u) != 0x19930520u) return 1;
    int32_t *pstate = (int32_t *)(uintptr_t)(frame + 8);
    uint32_t ebp = frame + 0xc;
    const uint32_t *unwind = (const uint32_t *)(uintptr_t)fi[2];
    if (er[1] & 6) {                                    /* unwinding: run this frame's destructors */
        int32_t st = *pstate;
        while (st >= 0 && st < (int32_t)fi[1]) { int32_t to = (int32_t)unwind[st * 2]; uint32_t a = unwind[st * 2 + 1]; *pstate = to; if (a) eh_call_ebp(a, ebp); st = to; }
        return 1;
    }
    const int cxx = er[0] == 0xe06d7363u && er[4] >= 3 && er[5] == 0x19930520u && er[7];
    const uint32_t *ti = cxx ? (const uint32_t *)(uintptr_t)er[7] : NULL;
    int32_t cur = *pstate;
    const uint32_t *tb = (const uint32_t *)(uintptr_t)fi[4];
    for (uint32_t i = 0; i < fi[3]; i++, tb += 5) {
        int32_t lo = (int32_t)tb[0], hi = (int32_t)tb[1];
        if (cur < lo || cur > hi) continue;
        const uint32_t *h = (const uint32_t *)(uintptr_t)tb[4];
        for (uint32_t k = 0; k < tb[3]; k++, h += 4) {
            const char *td = (const char *)(uintptr_t)h[1];
            const uint32_t *ct = NULL;
            if (td && td[8]) {                          /* typed catch: only for a C++ throw of that type */
                if (!cxx || !ti[3]) continue;
                const uint32_t *cta = (const uint32_t *)(uintptr_t)ti[3];
                for (uint32_t j = 0; j < cta[0]; j++) {
                    const uint32_t *c2 = (const uint32_t *)(uintptr_t)cta[1 + j];
                    if (c2 && c2[1] && !strcmp((const char *)(uintptr_t)(c2[1] + 8), td + 8)) { ct = c2; break; }
                }
                if (!ct) continue;
            }
            eh_log(cxx ? "catch (C++)" : "catch (...)", er);
            if (cxx && ct && h[2]) {                    /* hand the thrown object to the catch variable */
                uint8_t *obj = (uint8_t *)(uintptr_t)er[6] + (int32_t)ct[2];
                uint8_t *dst = (uint8_t *)(uintptr_t)(ebp + (int32_t)h[2]);
                if (h[0] & 8) *(uint32_t *)dst = (uint32_t)(uintptr_t)obj;          /* by reference */
                else memcpy(dst, obj, ct[5] ? ct[5] : 4);                           /* by value */
            }
            set_except_list(frame);                     /* frames nested inside are abandoned */
            int32_t st = cur;                           /* destructors of the try block */
            while (st > lo && st >= 0) { int32_t to = (int32_t)unwind[st * 2]; uint32_t a = unwind[st * 2 + 1]; *pstate = to; if (a) eh_call_ebp(a, ebp); st = to; }
            *pstate = hi + 1;
            uint32_t cont = eh_call_ebp(h[3], ebp);
            *pstate = lo - 1;
            eh_resume(*(uint32_t *)(uintptr_t)(frame - 4), ebp, cont);
        }
    }
    return 1;
}
static int CDECL c_except_handler3(uint32_t *er, uint32_t frame, uint32_t *ctx, void *disp) {
    (void)disp;
    uint32_t ebp = frame + 0x10;
    const uint32_t *scope = (const uint32_t *)(uintptr_t)*(uint32_t *)(uintptr_t)(frame + 8);
    int32_t *ptl = (int32_t *)(uintptr_t)(frame + 0xc);
    if (er[1] & 6) {                                    /* unwinding: run __finally blocks */
        while (*ptl != -1) { int32_t tl = *ptl; *ptl = (int32_t)scope[tl * 3]; if (!scope[tl * 3 + 1]) eh_call_ebp(scope[tl * 3 + 2], ebp); }
        return 1;
    }
    static uint32_t xp[2]; xp[0] = (uint32_t)(uintptr_t)er; xp[1] = (uint32_t)(uintptr_t)ctx;
    *(uint32_t *)(uintptr_t)(ebp - 0x14) = (uint32_t)(uintptr_t)xp;
    for (int32_t tl = *ptl; tl != -1; tl = (int32_t)scope[tl * 3]) {
        if (!scope[tl * 3 + 1]) continue;               /* __finally */
        int32_t r = (int32_t)eh_call_ebp(scope[tl * 3 + 1], ebp);
        if (r < 0) return 0;                            /* EXCEPTION_CONTINUE_EXECUTION */
        if (r == 0) continue;
        eh_log("__except", er);
        set_except_list(frame);
        while (*ptl != tl) { int32_t t2 = *ptl; *ptl = (int32_t)scope[t2 * 3]; if (!scope[t2 * 3 + 1]) eh_call_ebp(scope[t2 * 3 + 2], ebp); }
        *ptl = (int32_t)scope[tl * 3];
        eh_resume(*(uint32_t *)(uintptr_t)(ebp - 0x18), ebp, scope[tl * 3 + 2]);
    }
    return 1;
}
static void CDECL c_type_info_dtor(void) {}
static size_t CDECL c_wcslen(const uint16_t *s) { size_t n = 0; while (s[n]) n++; return n; }
static uint16_t g_ctype[257];
static const uint16_t *g_pctype = g_ctype + 1;
static int g_mb_cur_max = 1;
static int CDECL c_isctype(int c, int m) { return (c >= -1 && c < 256) ? (g_ctype[c + 1] & m) : 0; }
static void ctype_init(void) {
    for (int c = 0; c < 256; c++) { uint16_t v = 0;
        if (c < 128) { if (isupper(c)) v |= 1; if (islower(c)) v |= 2; if (isdigit(c)) v |= 4; if (isspace(c)) v |= 8; if (ispunct(c)) v |= 0x10; if (iscntrl(c)) v |= 0x20; if (c == ' ') v |= 0x40; if (isxdigit(c)) v |= 0x80; if (isalpha(c)) v |= 0x100; }
        g_ctype[c + 1] = v; }
}
/* x87 helpers */
__attribute__((naked, used)) static void c_ftol(void) {
    __asm__ volatile("sub $12, %esp\n fnstcw (%esp)\n mov (%esp), %ax\n or $0x0c00, %ax\n mov %ax, 2(%esp)\n fldcw 2(%esp)\n"
                     "fistpll 4(%esp)\n fldcw (%esp)\n mov 4(%esp), %eax\n mov 8(%esp), %edx\n add $12, %esp\n ret\n");
}
__attribute__((naked, used)) static void c_CIacos(void) {
    __asm__ volatile("fld %st(0)\n fmul %st(0), %st(0)\n fld1\n fsubp\n fsqrt\n fxch\n fpatan\n ret\n");
}
__attribute__((naked, used)) static void c_CIasin(void) {   /* asin(x) = atan2(x, sqrt(1-x^2)) */
    __asm__ volatile("fld %st(0)\n fmul %st(0), %st(0)\n fld1\n fsubp\n fsqrt\n fpatan\n ret\n");
}
__attribute__((naked, used)) static void c_CIsqrt(void) { __asm__ volatile("fsqrt\n ret\n"); }
__attribute__((naked, used)) static void c_CIsin(void) { __asm__ volatile("fsin\n ret\n"); }
__attribute__((naked, used)) static void c_CIcos(void) { __asm__ volatile("fcos\n ret\n"); }
__attribute__((naked, used)) static void c_CIatan2(void) { __asm__ volatile("fpatan\n ret\n"); }   /* st1 = y, st0 = x */
__attribute__((naked, used)) static void c_CIpow(void) {   /* st1 = x, st0 = y -> x^y via the C library */
    __asm__ volatile("sub $16, %esp\n fstpl 8(%esp)\n fstpl (%esp)\n call pow_cdecl\n add $16, %esp\n ret\n");
}
__attribute__((used, cdecl)) double pow_cdecl(double x, double y) { return pow(x, y); }

/* ------------------------------------------------------------------ */
/* gdi32 / user32 pieces (GPxPDX7 text on surfaces is drawn by its own fonts; these are fallbacks) */
static UINT WINAPI GetPaletteEntries_(void *p, UINT s, UINT n, void *e) { (void)p; (void)s; memset(e, 0, n * 4); return n; }
extern int gdi_draw_text(uint32_t dc, const char *s, int n, int32_t *rc, UINT fl);
static int WINAPI DrawTextA_(uint32_t dc, const char *s, int n, int32_t *r, UINT f) { return gdi_draw_text(dc, s, n, r, f); }
static BOOL WINAPI GetClassInfoA_(void *i, const char *c, void *wc) { (void)i; (void)c; memset(wc, 0, 40); return FALSE; }
static void *WINAPI GetMenu_(HWND h) { (void)h; return NULL; }
static HWND WINAPI GetParent_(HWND h) { (void)h; return 0; }
static BOOL WINAPI GetWindowPlacement_(HWND h, uint32_t *wp) { (void)h; memset(wp + 1, 0, 40); wp[0] = 44; wp[2] = 1; return TRUE; }
static BOOL WINAPI SetWindowPlacement_(HWND h, const void *wp) { (void)h; (void)wp; return TRUE; }

/* --- winmm multimedia file I/O (GPxSound reads its .wav files with it) ---------------------
   The whole file is read into memory; the MMIOINFO buffer handed out by mmioGetInfo is
   that memory, so the buffered-read loop of the DirectX SDK wave reader works as is. */
typedef struct { uint8_t *p; uint32_t n, pos; int own; } mmf_t;
typedef struct { uint32_t ckid, cksize, fcc, off, flags; } mmck_t;
static mmf_t *g_mmf[64];
#define MMIO_H(i) ((uint32_t)(0x6d000 + (i)))
static mmf_t *mmf_of(uint32_t h) { uint32_t i = h - 0x6d000; return i < 64 ? g_mmf[i] : NULL; }
static uint32_t WINAPI mmioOpenA_(const char *name, uint32_t *info, DWORD flags) {
    if (flags & 0x3) { LOG("mmioOpen(%s): write access not supported", name ? name : "(memory)"); if (info) info[3] = 257; return 0; }
    mmf_t *f = calloc(1, sizeof *f);
    if (!name && info && info[1] == 0x204d454du && info[6]) { f->p = (uint8_t *)(uintptr_t)info[6]; f->n = info[5]; }  /* FOURCC_MEM */
    else {
        char hp[1024];
        if (!name || !path_to_host(name, hp, sizeof hp)) { free(f); if (info) info[3] = 257; /* MMIOERR_FILENOTFOUND */ return 0; }
        FILE *fp = fopen(hp, "rb");
        if (!fp) { free(f); if (info) info[3] = 257; return 0; }
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        f->p = malloc(n > 0 ? n : 1); f->n = n > 0 ? (uint32_t)fread(f->p, 1, n, fp) : 0; f->own = 1; fclose(fp);
    }
    for (int i = 0; i < 64; i++) if (!g_mmf[i]) { g_mmf[i] = f; if (info) info[3] = 0; return MMIO_H(i); }
    if (f->own) free(f->p); free(f); return 0;
}
static UINT WINAPI mmioClose_(uint32_t h, UINT fl) { (void)fl; mmf_t *f = mmf_of(h); if (!f) return 5; if (f->own) free(f->p); free(f); g_mmf[h - 0x6d000] = NULL; return 0; }
static LONG WINAPI mmioRead_(uint32_t h, void *buf, LONG n) {
    mmf_t *f = mmf_of(h); if (!f || n < 0) return -1;
    uint32_t k = f->pos >= f->n ? 0 : f->n - f->pos; if ((uint32_t)n < k) k = (uint32_t)n;
    memcpy(buf, f->p + f->pos, k); f->pos += k; return (LONG)k;
}
static LONG WINAPI mmioSeek_(uint32_t h, LONG off, int org) {
    mmf_t *f = mmf_of(h); if (!f) return -1;
    int64_t np = org == 0 ? off : org == 1 ? (int64_t)f->pos + off : (int64_t)f->n + off;
    if (np < 0) return -1;
    f->pos = (uint32_t)np; return (LONG)f->pos;
}
static uint32_t rd32(mmf_t *f, uint32_t at) { uint32_t v = 0; if (at + 4 <= f->n) memcpy(&v, f->p + at, 4); return v; }
static UINT WINAPI mmioDescend_(uint32_t h, mmck_t *ck, const mmck_t *parent, UINT fl) {
    mmf_t *f = mmf_of(h); if (!f || !ck) return 5;
    uint32_t end = parent ? parent->off + parent->cksize : f->n;
    if (end > f->n) end = f->n;
    uint32_t want_id = ck->ckid, want_type = ck->fcc;
    for (;;) {
        if (f->pos + 8 > end) return 259;   /* MMIOERR_CHUNKNOTFOUND */
        uint32_t id = rd32(f, f->pos), sz = rd32(f, f->pos + 4), dat = f->pos + 8;
        int list = id == 0x46464952u /*RIFF*/ || id == 0x5453494cu /*LIST*/;
        uint32_t type = list ? rd32(f, dat) : 0;
        int match = (fl & 0x20) ? (id == 0x46464952u && type == want_type) : (fl & 0x40) ? (id == 0x5453494cu && type == want_type) : (fl & 0x10) ? id == want_id : 1;
        if (match) {
            ck->ckid = id; ck->cksize = sz; ck->fcc = type; ck->off = dat; ck->flags = 0;
            f->pos = dat + (list ? 4 : 0);
            return 0;
        }
        f->pos = dat + sz + (sz & 1);
    }
}
static UINT WINAPI mmioAscend_(uint32_t h, mmck_t *ck, UINT fl) {
    (void)fl; mmf_t *f = mmf_of(h); if (!f || !ck) return 5;
    f->pos = ck->off + ck->cksize + (ck->cksize & 1); return 0;
}
/* MMIOINFO: [6] pchBuffer, [7] pchNext, [8] pchEndRead, [9] pchEndWrite, [10] lBufOffset, [11] lDiskOffset, [17] hmmio */
static UINT WINAPI mmioGetInfo_(uint32_t h, uint32_t *info, UINT fl) {
    (void)fl; mmf_t *f = mmf_of(h); if (!f || !info) return 5;
    memset(info, 0, 72);
    info[5] = f->n; info[6] = (uint32_t)(uintptr_t)f->p; info[7] = (uint32_t)(uintptr_t)(f->p + (f->pos < f->n ? f->pos : f->n));
    info[8] = info[9] = (uint32_t)(uintptr_t)(f->p + f->n); info[10] = 0; info[11] = f->n; info[17] = h;
    return 0;
}
static UINT WINAPI mmioSetInfo_(uint32_t h, const uint32_t *info, UINT fl) {
    (void)fl; mmf_t *f = mmf_of(h); if (!f || !info) return 5;
    uint32_t nx = info[7] - (uint32_t)(uintptr_t)f->p; if (nx <= f->n) f->pos = nx; return 0;
}
static UINT WINAPI mmioAdvance_(uint32_t h, uint32_t *info, UINT fl) {
    (void)fl; mmf_t *f = mmf_of(h); if (!f) return 5;
    if (info) { uint32_t nx = info[7] - (uint32_t)(uintptr_t)f->p; if (nx <= f->n) f->pos = nx;
                info[7] = (uint32_t)(uintptr_t)(f->p + f->pos); info[8] = info[9] = (uint32_t)(uintptr_t)(f->p + f->n); }
    return 0;
}
static uint32_t WINAPI mmioStringToFOURCCA_(const char *s, UINT fl) {
    uint32_t v = 0; for (int i = 0; i < 4 && s[i]; i++) v |= (uint32_t)(uint8_t)((fl & 0x10) ? toupper((unsigned char)s[i]) : s[i]) << (8 * i);
    for (int i = (int)strlen(s); i < 4; i++) v |= (uint32_t)' ' << (8 * i);
    return v;
}

#define K(n) { "kernel32", #n, 0, (void *)n##_ }
#define C(n, f) { "msvcrt", n, 0, (void *)f }
#define G(n) { "gdi32", #n, 0, (void *)n##_ }
#define U(n) { "user32", #n, 0, (void *)n##_ }
static const export_t gpx_exports[] = {
    { "winmm", "mmioOpenA", 0, (void *)mmioOpenA_ }, { "winmm", "mmioClose", 0, (void *)mmioClose_ }, { "winmm", "mmioRead", 0, (void *)mmioRead_ },
    { "winmm", "mmioSeek", 0, (void *)mmioSeek_ }, { "winmm", "mmioDescend", 0, (void *)mmioDescend_ }, { "winmm", "mmioAscend", 0, (void *)mmioAscend_ },
    { "winmm", "mmioGetInfo", 0, (void *)mmioGetInfo_ }, { "winmm", "mmioSetInfo", 0, (void *)mmioSetInfo_ }, { "winmm", "mmioAdvance", 0, (void *)mmioAdvance_ },
    { "winmm", "mmioStringToFOURCCA", 0, (void *)mmioStringToFOURCCA_ },
    K(CreateFileMappingA), K(OpenFileMappingA), K(MapViewOfFile), K(UnmapViewOfFile), K(LocalAlloc), K(LocalFree), K(LocalLock), K(LocalUnlock),
    K(MulDiv), K(FlushInstructionCache), K(VirtualProtectEx), K(VirtualQueryEx), K(GetPrivateProfileSectionA), K(GetPrivateProfileSectionNamesA),
    K(WritePrivateProfileStringA), K(WritePrivateProfileSectionA), K(GetFullPathNameA), K(FormatMessageA), K(SetThreadContext), K(ReadProcessMemory), K(WriteProcessMemory), K(GetModuleHandleW), K(CopyFileA), { "rpcrt4", "UuidCreate", 0, (void *)UuidCreate_ },
    { "ntdll", "RtlGetVersion", 0, (void *)RtlGetVersion_ },
    { "wsock32", "WSAStartup", 115, (void *)WSAStartup_ }, { "wsock32", "WSACleanup", 116, (void *)WSACleanup_ }, { "wsock32", "WSAGetLastError", 111, (void *)WSAGetLastError_ },
    { "wsock32", "gethostbyname", 52, (void *)gethostbyname_ }, { "wsock32", "gethostname", 57, (void *)gethostname_ }, { "wsock32", "socket", 23, (void *)socket_ },
    { "wsock32", "closesocket", 3, (void *)closesocket_ }, { "wsock32", "inet_addr", 10, (void *)inet_addr_ }, { "wsock32", "htons", 9, (void *)htons_ }, { "wsock32", "htonl", 8, (void *)htonl_ },
    C("_iob", g_iob), C("fprintf", c_fprintf), C("vfprintf", c_vfprintf), C("fputs", c_fputs), C("fputc", c_fputc), C("fflush", c_fflush),
    C("printf", c_printf), C("vsprintf", c_vsprintf), C("_snprintf", c_snprintf), C("??2@YAPAXI@Z", c_new), C("??3@YAXPAX@Z", c_delete),
    C("_purecall", c_purecall), C("_initterm", c_initterm), C("_onexit", c_onexit), C("__dllonexit", c_dllonexit), C("_adjust_fdiv", &g_adjust_fdiv),
    C("_finite", c_finite), C("_isnan", c_isnan), C("_strdup", c_strdup), C("iscntrl", c_iscntrl), C("isalpha", c_isalpha), C("isdigit", c_isdigit),
    C("isspace", c_isspace), C("isalnum", c_isalnum), C("isupper", c_isupper), C("islower", c_islower), C("isprint", c_isprint), C("isxdigit", c_isxdigit),
    C("tolower", c_tolower), C("toupper", c_toupper), C("time", c_time), C("localtime", c_localtime), C("gmtime", c_gmtime), C("strftime", c_strftime),
    C("setlocale", c_setlocale), C("strerror", c_strerror), C("strtod", c_strtod), C("strtok", c_strtok), C("_errno", c_errno), C("_access", c_access),
    C("_gcvt", c_gcvt), C("_ltoa", c_ltoa), C("_ultoa", c_ultoa), C("_itoa", c_itoa), C("_splitpath", c_splitpath), C("_makepath", c_makepath),
    C("_strlwr", c_strlwr), C("_strupr", c_strupr), C("rand", c_rand), C("srand", c_srand), C("qsort", c_qsort), C("sin", c_sin), C("cos", c_cos),
    C("atan2", c_atan2), C("fmod", c_fmod), C("labs", c_labs), C("remove", c_remove), C("rename", c_rename), C("_mkdir", c_mkdir), C("_stat", c_stat),
    C("ungetc", c_ungetc), C("fgetc", c_fgetc), C("feof", c_feof), C("ferror", c_ferror), C("rewind", c_rewind), C("fscanf", c_fscanf), C("puts", c_puts),
    C("strpbrk", c_strpbrk), C("strspn", c_strspn), C("strcspn", c_strcspn), C("strncat", c_strncat), C("memchr", c_memchr), C("atexit", c_atexit),
    C("abort", c_abort), C("_CxxThrowException", c_CxxThrowException), C("__CxxFrameHandler", c_CxxFrameHandler), C("??1type_info@@UAE@XZ", c_type_info_dtor),
    C("_except_handler3", c_except_handler3), C("wcslen", c_wcslen), C("_isctype", c_isctype), C("_pctype", &g_pctype), C("__mb_cur_max", &g_mb_cur_max),
    C("_ftol", c_ftol), C("_CIacos", c_CIacos), C("_CIasin", c_CIasin), C("_CIsqrt", c_CIsqrt), C("_CIsin", c_CIsin), C("_CIcos", c_CIcos),
    C("_CIatan2", c_CIatan2), C("_CIpow", c_CIpow),
    G(GetPaletteEntries),
    U(DrawTextA), U(GetClassInfoA), U(GetMenu), U(GetParent), U(GetWindowPlacement), U(SetWindowPlacement),
    { NULL, NULL, 0, NULL }
};
void gpx_api_register(void) { ctype_init(); rt_register_exports(gpx_exports); (void)GetFileAttributesEx_unused; }

/* GPxPSup 4.52 keeps a per-car timing table (GPxP_GetTimingInfo) whose car pointers it
   only fills for cars GP3 has flagged in its per-car byte array at 0xb94072 (bit 7). In
   a quick race no car carries that flag, every pointer stays NULL and GPxSat's standings
   pages (B, F, E) fault on the first one; GPxPatch then disables GPxSat for the session.
   The flag test is removed so every running car gets its entry. Applied only when the
   exact GPxPSup 4.52 code is found. */
/* GPxPatch tells its modules that a race session starts (OnStartSession) from a hook on
   one of the menu paths of GP3 that start a session (in GP3 1.13: the call to 0x487c4a at
   0x47e58a). The quick race arrives through another path that calls the same routine, so
   GPxSat's race displays ran without their session data (standings/tower faulted and
   GPxSat was switched off). The other calls of that routine are routed through the same
   GPx stub. Found by pattern, so it works for both game versions. */
static uint32_t pe_export_addr(uint32_t base, const char *want) {
    const uint8_t *m = (const uint8_t *)(uintptr_t)base;
    uint32_t pe = *(const uint32_t *)(m + 0x3c), ed = *(const uint32_t *)(m + pe + 0x78);
    if (!ed) return 0;
    const uint32_t *d = (const uint32_t *)(m + ed);
    uint32_t nn = d[6], af = d[7], an = d[8], ao = d[9];
    for (uint32_t i = 0; i < nn; i++) {
        const char *n = (const char *)(m + ((const uint32_t *)(m + an))[i]);
        if (!strcmp(n, want)) return base + ((const uint32_t *)(m + af))[((const uint16_t *)(m + ao))[i]];
    }
    return 0;
}
static void gpx_session_hooks(void) {
    extern module_t *g_modules;
    module_t *sup = NULL, *gxp = NULL;
    for (module_t *m = g_modules; m; m = m->next) {
        if (strstr(m->name, "gpxpsup")) sup = m;
        else if (strstr(m->name, "gpxpatch.gxm")) gxp = m;
    }
    if (!sup || !gxp) return;
    uint32_t oss = pe_export_addr(sup->base, "?_GPxP_OnStartSession@@YAX_N@Z");
    if (!oss) { LOG("GPxPatch: OnStartSession not exported: session paths left alone"); return; }
    const uint8_t *g = (const uint8_t *)(uintptr_t)gxp->base;
    /* the IAT slot of gpxpatch.gxm holding OnStartSession */
    uint32_t slot = 0;
    for (uint32_t o = 0; o + 4 <= gxp->size && !slot; o += 4) if (*(const uint32_t *)(g + o) == oss) slot = gxp->base + o;
    if (!slot) { LOG("GPxPatch: OnStartSession import not found"); return; }
    /* stubs: E8 <orig> 90 9C 60 E8 03000000 61 9D C3 55 8B EC 6A 01 E8 <dispatcher>, dispatcher calls [slot] */
    uint32_t stub = 0, orig = 0;
    for (uint32_t o = 0; o + 32 < gxp->size; o++) {
        const uint8_t *q = g + o;
        if (q[0] != 0xe8 || q[5] != 0x90 || q[6] != 0x9c || q[7] != 0x60 || q[8] != 0xe8 || q[9] != 3 || q[13] != 0x61 || q[14] != 0x9d || q[15] != 0xc3
            || q[16] != 0x55 || q[17] != 0x8b || q[18] != 0xec || q[19] != 0x6a || q[20] != 0x01 || q[21] != 0xe8) continue;
        uint32_t at = gxp->base + o, disp = at + 26 + *(const int32_t *)(q + 22);
        if (disp < gxp->base || disp >= gxp->base + gxp->size - 0x60) continue;
        int calls = 0;
        for (const uint8_t *r = (const uint8_t *)(uintptr_t)disp; r < (const uint8_t *)(uintptr_t)disp + 0x60; r++)
            if (r[0] == 0xff && r[1] == 0x15 && *(const uint32_t *)(r + 2) == slot) { calls = 1; break; }
        if (!calls) continue;
        stub = at; orig = at + 5 + *(const int32_t *)(q + 1); break;
    }
    if (!stub) { LOG("GPxPatch: race session hook not found: other race start paths left alone"); return; }
    /* every other call of the same game routine in GP3's code section */
    const uint8_t *x = (const uint8_t *)(uintptr_t)0x400000;
    uint32_t pe = *(const uint32_t *)(x + 0x3c);
    uint32_t tva = *(const uint32_t *)(x + pe + 0x2c), tsz = *(const uint32_t *)(x + pe + 0x1c);   /* BaseOfCode, SizeOfCode */
    uint32_t sites[8]; int ns = 0, hooked = 0;
    for (uint32_t a = 0x400000 + tva; a + 5 < 0x400000 + tva + tsz; a++) {
        const uint8_t *c = (const uint8_t *)(uintptr_t)a;
        if (c[0] != 0xe8) continue;
        uint32_t t = a + 5 + *(const int32_t *)(c + 1);
        if (t == stub) hooked++;
        else if (t == orig) { if (ns < 8) sites[ns] = a; ns++; }
    }
    if (hooked != 1 || ns == 0 || ns > 6) { LOG("GPxPatch: race session paths: %d hooked, %d others: left alone", hooked, ns); return; }
    int n = 0;
    for (int i = 0; i < ns; i++) {
        uint8_t p[5]; p[0] = 0xe8; int32_t rel = (int32_t)(stub - (sites[i] + 5)); memcpy(p + 1, &rel, 4);
        extern int plat_patch_code(void *addr, const void *bytes, int n);
        if (plat_patch_code((void *)(uintptr_t)sites[i], p, 5) == 0) n++;
    }
    LOG("GPxPatch: race session start reported to the modules on %d more menu path(s) (routine %08x)", n, orig);
}
void gpx_fixups(void) {
    extern module_t *g_modules;
    if (!getenv("GP3_GPX_NOSESSION")) gpx_session_hooks();
    for (module_t *m = g_modules; m; m = m->next) {
        if (strcasecmp(m->name, "gpxpsup.dll") && !strstr(m->name, "gpxpsup")) continue;
        static const uint8_t want[] = { 0x80, 0x3c, 0x28, 0x00, 0x0f, 0x8d, 0xa7, 0x00, 0x00, 0x00 };
        uint8_t *p = (uint8_t *)(uintptr_t)(m->base + 0x8cb2);
        if (m->size > 0x8cc0 && !memcmp(p, want, sizeof want)) {
            memset(p + 4, 0x90, 6);
            LOG("GPxPatch: GPxPSup timing table fix applied (standings/tower in quick races)");
        } else LOG("GPxPatch: GPxPSup is not version 4.52: timing table fix not applied");
    }
}
