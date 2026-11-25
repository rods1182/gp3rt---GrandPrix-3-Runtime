/* Core services: logging, handle table, virtual memory, path translation,
 * timing and the program entry point of the native runner. */
#include "rt.h"
#include "gameexe.h"
char g_exe_file[256] = "Gp3.exe";   /* its name in the game folder */
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <ctype.h>

int g_log_level = 1;
int g_trace_d3d;
char g_capture_path[512];
FILE *g_log;
config_t g_cfg;
char g_game_dir[1024];
static pthread_mutex_t g_log_mx = PTHREAD_MUTEX_INITIALIZER;

void rt_log(int lvl, const char *fmt, ...) {
    if (lvl > g_log_level) return;
    FILE *f = g_log ? g_log : stderr;
    va_list ap; va_start(ap, fmt);
    pthread_mutex_lock(&g_log_mx);
    fprintf(f, "[%8.3f] ", rt_now_us() / 1e6);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    if (lvl == 0 || g_log_level >= 2) fflush(f);
    pthread_mutex_unlock(&g_log_mx);
    va_end(ap);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    FILE *f = g_log ? g_log : stderr;
    fprintf(f, "FATAL: "); vfprintf(f, fmt, ap); fputc('\n', f);
    if (f != stderr) { va_end(ap); va_start(ap, fmt); fprintf(stderr, "FATAL: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); }
    va_end(ap);
    fflush(f);
    _exit(2);
}

/* ------------------------------------------------------------------ */
/* time                                                                 */
static uint64_t g_t0;
uint64_t rt_now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
    if (!g_t0) g_t0 = t - 1000000;   /* start at 1s so GetTickCount is never 0 */
    return t - g_t0;
}
uint32_t rt_ticks_ms(void) { return (uint32_t)(rt_now_us() / 1000); }
void os_sleep_us(uint64_t us) {
    struct timespec ts = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

/* ------------------------------------------------------------------ */
/* handles                                                              */
#define MAX_HANDLES 4096
static hobj_t *g_handles[MAX_HANDLES];
static pthread_mutex_t g_hmx = PTHREAD_MUTEX_INITIALIZER;

HANDLE handle_new(int type, void *p, void (*close)(hobj_t *)) {
    pthread_mutex_lock(&g_hmx);
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!g_handles[i]) {
            hobj_t *o = calloc(1, sizeof *o);
            o->type = type; o->p = p; o->close = close; o->refs = 1;
            g_handles[i] = o;
            pthread_mutex_unlock(&g_hmx);
            return (HANDLE)(i * 4);
        }
    pthread_mutex_unlock(&g_hmx);
    return 0;
}
hobj_t *handle_get(HANDLE h, int type) {
    uint32_t i = h / 4;
    if (h & 3 || i == 0 || i >= MAX_HANDLES) return NULL;
    hobj_t *o = g_handles[i];
    if (!o || (type && o->type != type)) return NULL;
    return o;
}
BOOL handle_close(HANDLE h) {
    pthread_mutex_lock(&g_hmx);
    uint32_t i = h / 4;
    hobj_t *o = (h & 3 || !i || i >= MAX_HANDLES) ? NULL : g_handles[i];
    if (o) g_handles[i] = NULL;
    pthread_mutex_unlock(&g_hmx);
    if (!o) return FALSE;
    if (o->close) o->close(o);
    free(o);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* memory: keep everything below 2GB, the game is not large-address-aware */
void mem_reserve_high(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    uint32_t prev_end = 0x7fff0000u;
    char line[512];
    uint32_t ranges[256][2]; int n = 0;
    while (fgets(line, sizeof line, f) && n < 256) {
        unsigned long a, b;
        if (sscanf(line, "%lx-%lx", &a, &b) == 2) { ranges[n][0] = a; ranges[n][1] = b; n++; }
    }
    fclose(f);
    for (int i = 0; i < n; i++) {
        uint32_t a = ranges[i][0], b = ranges[i][1];
        if (b <= prev_end) continue;
        if (a > prev_end) {
            uint32_t gap_end = a;
            /* leave room for the main stack to grow */
            if (gap_end - prev_end > 0x01000000u && a >= 0xff000000u) gap_end = a - 0x01000000u;
            if (gap_end > prev_end)
                mmap((void *)(uintptr_t)prev_end, gap_end - prev_end, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        }
        prev_end = b;
    }
}

void *rt_valloc(uint32_t addr, uint32_t size, int commit) {
    (void)commit;
    size = (size + 0xfff) & ~0xfffu;
    int fl = MAP_PRIVATE | MAP_ANONYMOUS;
    if (addr) fl |= MAP_FIXED_NOREPLACE;
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE | PROT_EXEC, fl, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}
void rt_vfree(void *p, uint32_t size) { munmap(p, size); }

/* page-aligned surface memory + protection for lazy render-target readback */
void *surf_mem_alloc(size_t n) {
    n = (n + 4095) & ~(size_t)4095;
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
int plat_write_watch(void *base, size_t len, int reset, void **pages, size_t *n) { (void)base; (void)len; (void)reset; (void)pages; (void)n; return -1; }
void surf_mem_free(void *p, size_t n) { if (p) munmap(p, (n + 4095) & ~(size_t)4095); }
int plat_protect(void *p, size_t n, int mode) {
    return mprotect(p, n, mode == 0 ? PROT_NONE : mode == 1 ? PROT_READ : PROT_READ | PROT_WRITE);
}
uintptr_t plat_module_of(void *addr, char *name, int n) {
    extern const char *rt_module_name_of(uint32_t a, uint32_t *base);
    uint32_t base = 0; const char *nm = rt_module_name_of((uint32_t)(uintptr_t)addr, &base);
    snprintf(name, n, "%s", nm ? nm : "?");
    return base ? base : ((uintptr_t)addr & ~0xffffu);
}
int plat_make_writable(void *addr, int n) {
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)4095, e = ((uintptr_t)addr + n + 4095) & ~(uintptr_t)4095;
    return mprotect((void *)a, e - a, PROT_READ | PROT_WRITE | PROT_EXEC);
}
int plat_patch_code(void *addr, const void *bytes, int n) {
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)4095, e = ((uintptr_t)addr + n + 4095) & ~(uintptr_t)4095;
    if (mprotect((void *)a, e - a, PROT_READ | PROT_WRITE | PROT_EXEC)) return -1;
    memcpy(addr, bytes, n);
    return 0;
}

/* ------------------------------------------------------------------ */
/* paths                                                                */
void cp1252_to_utf8(const char *in, char *out, size_t outsz) {
    static const uint16_t hi[32] = {0x20AC,0x81,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,0x2C6,0x2030,0x160,0x2039,0x152,0x8D,0x17D,0x8F,
                                    0x90,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x2DC,0x2122,0x161,0x203A,0x153,0x9D,0x17E,0x178};
    size_t o = 0;
    for (; *in && o + 4 < outsz; in++) {
        unsigned c = (unsigned char)*in;
        if (c >= 0x80 && c < 0xa0) c = hi[c - 0x80];
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = 0xc0 | (c >> 6); out[o++] = 0x80 | (c & 63); }
        else { out[o++] = 0xe0 | (c >> 12); out[o++] = 0x80 | ((c >> 6) & 63); out[o++] = 0x80 | (c & 63); }
    }
    out[o] = 0;
}

/* case-insensitive (ASCII + Latin-1 in UTF-8) compare */
static int name_ieq(const char *a, const char *b) {
    while (*a && *b) {
        unsigned x = (unsigned char)*a, y = (unsigned char)*b;
        if (x < 128 && y < 128) { if (tolower(x) != tolower(y)) return 0; a++; b++; continue; }
        /* 2-byte UTF-8 Latin-1 letters: upper/lower differ by 0x20 in the code point */
        if (x == 0xc3 && y == 0xc3 && a[1] && b[1]) {
            unsigned u = (unsigned char)a[1], v = (unsigned char)b[1];
            if (u >= 0x80 && u <= 0x9e) u += 0x20;
            if (v >= 0x80 && v <= 0x9e) v += 0x20;
            if (u != v) return 0;
            a += 2; b += 2; continue;
        }
        if (x != y) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* one "character" of a file name: ASCII byte, a whole UTF-8 sequence, or one stray byte
   (names extracted from old zip/rar archives keep Latin-1 or DOS code page bytes) */
static int name_unit(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    unsigned c = *s;
    if (c < 0x80) {
        (*p)++;
        /* decomposed accents (NFD): a letter followed by combining marks U+0300..U+036F */
        int comb = 0;
        while (((*p)[0] == (char)0xcc && ((unsigned char)(*p)[1] & 0xc0) == 0x80) ||
               ((*p)[0] == (char)0xcd && (unsigned char)(*p)[1] >= 0x80 && (unsigned char)(*p)[1] <= 0xaf)) { *p += 2; comb = 1; }
        return comb ? -1 : (int)tolower(c);
    }
    int n = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
    for (int i = 1; i < n; i++) if ((s[i] & 0xc0) != 0x80) { n = 1; break; }
    *p += n;
    return -1;   /* non-ASCII: matches any non-ASCII (or '?') character */
}
/* last resort: accented letters may be stored in another encoding than the game asks for
   ("português.str" asked in Windows-1252, stored as UTF-8, Latin-1 or a DOS code page) */
static int name_fuzzy(const char *a, const char *b) {
    while (*a && *b) {
        int x = name_unit(&a), y = name_unit(&b);
        if (x == y && x >= 0) continue;
        if ((x < 0 || x == '?') && (y < 0 || y == '?')) continue;
        return 0;
    }
    return *a == *b;
}

static void peek_log(void) {
    /* analysis: GP3_PEEK="0x1234,*0x5678,**0x9abc" logs 256 bytes at each address (each * dereferences once) */
    const char *e = getenv("GP3_PEEK"); if (!e) return;
    char buf[512]; snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        int n = 0; while (*t == '*') { n++; t++; }
        char *q; uint32_t a = strtoul(t, &q, 0);
        while (n-- && a) a = *(volatile uint32_t *)(uintptr_t)a;
        while (*q == ':' && a) {   /* chain: ":*" dereference, ":+n" add */
            q++;
            if (*q == '*') { a = *(volatile uint32_t *)(uintptr_t)a; q++; }
            else if (*q == '+') a += strtoul(q + 1, &q, 0);
            else break;
        }
        if (!a) { LOG("peek %s: null", t); continue; }
        for (int r = 0; r < 256; r += 32) {
            char l[200]; int k = 0;
            for (int i = 0; i < 32; i += 4) k += snprintf(l + k, sizeof l - k, " %08x", *(volatile uint32_t *)(uintptr_t)(a + r + i));
            LOG("peek %08x:%s", a + r, l);
        }
    }
}
void gpx_dump_modules(void) {
    peek_log();
    if (!getenv("GP3_GPX_DUMP")) return;   /* analysis: write the unpacked (UPX) images */
    extern module_t *g_modules;
    for (module_t *q = g_modules; q; q = q->next) {
        char fn[512]; const char *b = strrchr(q->name, '\\'); b = b ? b + 1 : q->name;
        snprintf(fn, sizeof fn, "%s/%s_%08x.img", getenv("GP3_GPX_DUMP"), b, q->base);
        FILE *f = fopen(fn, "wb"); if (f) { fwrite((void *)(uintptr_t)q->base, 1, q->size, f); fclose(f); }
    }
}
/* valid UTF-8 with at least one multi-byte character (an .ini saved as UTF-8 instead of Windows-1252) */
static int is_utf8_multibyte(const char *s) {
    int multi = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        if (*p < 0x80) { p++; continue; }
        int n = (*p & 0xe0) == 0xc0 ? 2 : (*p & 0xf0) == 0xe0 ? 3 : (*p & 0xf8) == 0xf0 ? 4 : 0;
        if (!n || *p < 0xc2) return 0;
        for (int i = 1; i < n; i++) if ((p[i] & 0xc0) != 0x80) return 0;
        p += n; multi = 1;
    }
    return multi;
}
static int path_to_host_u(const char *u_in, char *out, size_t outsz);
int path_to_host(const char *win, char *out, size_t outsz) {
    char u[1024];
    cp1252_to_utf8(win, u, sizeof u);
    if (path_to_host_u(u, out, outsz)) return 1;
    /* "França" written in UTF-8 inside a .set/.ini: GP3 passes the UTF-8 bytes as if they were Windows-1252 */
    if (is_utf8_multibyte(win)) { char o2[1024]; if (path_to_host_u(win, o2, sizeof o2)) { snprintf(out, outsz, "%s", o2); return 1; } }
    return 0;
}
static int path_to_host_u(const char *u_in, char *out, size_t outsz) {
    char u[1024];
    snprintf(u, sizeof u, "%s", u_in);
    for (char *p = u; *p; p++) if (*p == '\\') *p = '/';
    const char *rel = u;
    char base[1024];
    snprintf(base, sizeof base, "%s", g_game_dir);
    if (isalpha((unsigned char)u[0]) && u[1] == ':') {
        rel = u + 2;
        while (*rel == '/') rel++;
        int drv = tolower((unsigned char)u[0]);
        if (drv == 'c' && !strncasecmp(rel, "gp3/", 4)) rel += 4;
        else if (drv == 'c' && !strcasecmp(rel, "gp3")) rel += 3;
        else if (drv == VIRTUAL_CD_DRIVE) {
            /* virtual game CD: "\Grand Prix 3\..." and the root both map onto the installed game */
            if (!strncasecmp(rel, "grand prix 3/", 13)) rel += 13;
            else if (!strcasecmp(rel, "grand prix 3")) rel += 12;
        }
        else if (drv == 'z') snprintf(base, sizeof base, "%s", "");   /* Z:\ = the Linux file system root, as in Wine (tracks kept outside the game folder) */
        else snprintf(base, sizeof base, "%s/.drive_%c", g_game_dir, drv);
    } else if (u[0] == '/') {
        rel = u + 1;
    }
    char cur[1024];
    snprintf(cur, sizeof cur, "%s", base);
    int exists = 1;
    char comp[256];
    while (*rel) {
        const char *s = strchr(rel, '/');
        size_t n = s ? (size_t)(s - rel) : strlen(rel);
        if (n >= sizeof comp) n = sizeof comp - 1;
        memcpy(comp, rel, n); comp[n] = 0;
        rel += n; while (*rel == '/') rel++;
        /* strip trailing spaces/dots like Windows does */
        while (n && (comp[n - 1] == ' ' || comp[n - 1] == '.') && strcmp(comp, ".") && strcmp(comp, "..")) comp[--n] = 0;
        if (!n || !strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) { char *sl = strrchr(cur, '/'); if (sl && strlen(cur) > strlen(g_game_dir)) *sl = 0; continue; }
        const char *pick = comp;
        char found[256];
        if (exists) {
            DIR *d = opendir(cur[0] ? cur : "/");
            int ok = 0;
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) if (name_ieq(e->d_name, comp)) { snprintf(found, sizeof found, "%s", e->d_name); ok = 1; break; }
                int odd = 0; for (const char *q = comp; *q; q++) if ((unsigned char)*q >= 0x80 || *q == '?') odd = 1;
                if (!ok && odd) {
                    rewinddir(d);
                    while ((e = readdir(d))) if (name_fuzzy(e->d_name, comp)) { snprintf(found, sizeof found, "%s", e->d_name); ok = 1; break; }
                }
                closedir(d);
            }
            if (ok) pick = found; else exists = 0;
        }
        size_t l = strlen(cur);
        snprintf(cur + l, sizeof cur - l, "/%s", pick);
    }
    snprintf(out, outsz, "%s", cur);
    return exists;
}

/* ------------------------------------------------------------------ */
/* entry point                                                          */
extern void win32_register_all(void);
extern void win32_set_cmdline(const char *s);
extern void video_init(void);
extern void script_start(void);

static void usage(void) {
    fprintf(stderr,
        "gp3run - native Grand Prix 3 runtime\n"
        "usage: gp3run [options] <game-dir> [game args]\n"
        "  --headless          no window; write frames as PNG\n"
        "  --dump-dir DIR      where to write frames (default: <game-dir>/screens)\n"
        "  --dump-every N      dump every Nth frame (headless, default 30)\n"
        "  --max-seconds N     quit after N seconds\n"
        "  --script FILE       timed input script (testing)\n"
        "  --log FILE          log file (default stderr)\n"
        "  --windowed          run in a window\n"
        "  -v / -vv            more logging\n");
    exit(1);
}

static int g_trap_selftest;
static const char *g_inject;   /* DLL loaded into the game before its entry point (GPxPatch emulation) */
#include "gamepatch.h"
#include <pthread.h>
#include <fcntl.h>
static void *force_mode_thread(void *arg) {
    int idx = *(int *)arg;
    int is2000 = *(volatile uint32_t *)(uintptr_t)0x49b8be == 0x7eee18;   /* cmpl $6,[mode index] in GP3 2000 */
    uint32_t a = is2000 ? 0x7eee18 : 0xb27a14, b = is2000 ? 0x7eee0c : 0xb27a08;
    for (;;) { *(volatile uint32_t *)(uintptr_t)a = (uint32_t)idx; *(volatile uint32_t *)(uintptr_t)b = (uint32_t)idx; usleep(20000); }
    return NULL;
}
/* diagnostics: GP3_WATCH=<hexbytes>,<len>,<seconds>: after <seconds>, find the bytes in memory
 * and log every instruction that touches the next <len> bytes to out/acc.bin */
uint32_t g_traps[16]; int g_ntraps;
/* analysis: GP3_BP="0x...,0x..." breakpoints, placed when the module holding the address is loaded */
uint32_t g_bp[16]; uint8_t g_bp_orig[16]; int g_nbp;
void bp_install_range(uint32_t lo, uint32_t hi) {
    const char *e = getenv("GP3_BP"); if (!e) return;
    char buf[256]; snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t && g_nbp < 16; t = strtok(NULL, ",")) {
        uint32_t a = strtoul(t, NULL, 0); if (a < lo || a >= hi) continue;
        g_bp_orig[g_nbp] = *(uint8_t *)(uintptr_t)a; uint8_t cc = 0xcc;
        if (plat_patch_code((void *)(uintptr_t)a, &cc, 1) == 0) { g_bp[g_nbp++] = a; LOG("bp %08x set", a); }
    }
}
void traps_install(void) {
    const char *e = getenv("GP3_TRAP"); if (!e) return;
    char buf[256]; snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ","); t && g_ntraps < 16; t = strtok(NULL, ",")) {
        uint32_t a = strtoul(t, NULL, 0); uint8_t *p = (uint8_t *)(uintptr_t)a;
        if (*p != 0x90) { LOG("trap %08x: not a nop (%02x), skipped", a, *p); continue; }
        uint8_t cc = 0xcc; if (plat_patch_code(p, &cc, 1) == 0) { g_traps[g_ntraps++] = a; LOG("trap %08x set", a); }
    }
}
static void *poll_thread(void *arg) {
    /* analysis: GP3_POLL="addr:len,addr:len" logs whenever those bytes change (5 ms polling) */
    (void)arg; uint32_t a[8], l[8]; uint8_t last[8][32]; int n = 0;
    char *spec = strdup(getenv("GP3_POLL"));
    for (char *t = strtok(spec, ","); t && n < 8; t = strtok(NULL, ",")) { char *q; a[n] = strtoul(t, &q, 0); l[n] = *q == ':' ? strtoul(q + 1, NULL, 0) : 4; if (l[n] > 32) l[n] = 32; memset(last[n], 0xee, 32); n++; }
    for (;;) {
        for (int i = 0; i < n; i++) {
            uint8_t cur[32]; int fd = open("/proc/self/mem", O_RDONLY); ssize_t r = pread(fd, cur, l[i], a[i]); close(fd);
            if (r == (ssize_t)l[i] && memcmp(cur, last[i], l[i])) {
                char hx[100]; int k = 0; for (uint32_t j = 0; j < l[i]; j++) k += snprintf(hx + k, sizeof hx - k, "%02x", cur[j]);
                LOG("poll %08x: %s", a[i], hx); memcpy(last[i], cur, l[i]);
            }
        }
        struct timespec ts = { 0, 5000000 }; nanosleep(&ts, NULL);
    }
    return NULL;
}
static void *watch_thread(void *arg) {
    (void)arg;
    char *spec = strdup(getenv("GP3_WATCH")); char *hx = strtok(spec, ","); size_t len = strtoul(strtok(NULL, ","), NULL, 0); int sec = atoi(strtok(NULL, ","));
    uint8_t pat[256]; size_t pl = strlen(hx) / 2; for (size_t i = 0; i < pl && i < 256; i++) sscanf(hx + 2 * i, "%2hhx", &pat[i]);
    sleep(sec);
    uint8_t *hit = NULL;
    for (int tries = 0; !hit && tries < 2000; tries++) {
        FILE *m = fopen("/proc/self/maps", "r"); char line[512];
        while (!hit && fgets(line, sizeof line, m)) {
            unsigned long a, b; char perm[8];
            if (sscanf(line, "%lx-%lx %7s", &a, &b, perm) != 3 || perm[0] != 'r' || perm[1] != 'w' || a >= 0x20000000ul || b - a > 0x4000000) continue;   /* guest memory only (the runner lives at 0x20000000+) */
            /* read through /proc/self/mem: pages that are protected right now give an error instead of a fault */
            static uint8_t buf[1 << 20]; int fd = open("/proc/self/mem", O_RDONLY);
            for (unsigned long o = a; o < b && !hit; o += sizeof buf - 256) {
                size_t n = b - o < sizeof buf ? b - o : sizeof buf;
                ssize_t r = pread(fd, buf, n, (off_t)o); if (r <= 0) continue;
                for (ssize_t i = 0; i + (ssize_t)pl <= r; i++) if (buf[i] == pat[0] && !memcmp(buf + i, pat, pl)) { hit = (uint8_t *)(o + i); break; }
            }
            close(fd);
        }
        fclose(m);
        if (!hit) usleep(5000);
    }
    if (!hit) { LOG("watch: pattern not found"); return NULL; }
    extern uint8_t *g_acc_base; extern size_t g_acc_len; extern FILE *g_acc_file;
    uint8_t *pg = (uint8_t *)((uintptr_t)hit & ~(uintptr_t)4095); size_t pl2 = (((uintptr_t)hit + len + 4095) & ~(uintptr_t)4095) - (uintptr_t)pg;
    g_acc_file = fopen("/home/claude/gp3/out/acc.bin", "wb");
    LOG("watch: pattern at %p, watching %p..+%zx", (void *)hit, (void *)pg, pl2);
    g_acc_len = pl2; g_acc_base = pg; mprotect(pg, pl2, PROT_NONE);
    sleep(getenv("GP3_WATCH_SECS") ? atoi(getenv("GP3_WATCH_SECS")) : 4);
    g_acc_base = NULL; mprotect(pg, pl2, PROT_READ | PROT_WRITE); fflush(g_acc_file);
    LOG("watch: done");
    return NULL;
}
/* diagnostics: GP3_MEMDUMP=<addr>,<len>,<after s>,<file> - copy guest memory to a file (track editor research) */
static void *memdump_thread(void *arg) {
    (void)arg; unsigned long a = 0, l = 0; int st = 0; char fn[512] = "memdump.bin";
    sscanf(getenv("GP3_MEMDUMP"), "%li,%li,%d,%511s", (long *)&a, (long *)&l, &st, fn);
    sleep(st);
    uint8_t *buf = malloc(l); int fd = open("/proc/self/mem", O_RDONLY);
    ssize_t r = pread(fd, buf, l, (off_t)a); close(fd);
    FILE *f = fopen(fn, "wb"); if (f) { if (r > 0) fwrite(buf, 1, (size_t)r, f); fclose(f); }
    LOG("memdump: %lx+%lx -> %s (%zd bytes)", a, l, fn, r); free(buf);
    return NULL;
}
/* diagnostics: GP3_WATCH_ADDR=<addr>,<len>,<start s>,<duration s> */
static void *watch_addr_thread(void *arg) {
    (void)arg; unsigned long a = 0, l = 0; int st = 0, du = 5;
    sscanf(getenv("GP3_WATCH_ADDR"), "%li,%li,%d,%d", (long *)&a, (long *)&l, &st, &du);
    sleep(st);
    extern uint8_t *g_acc_base; extern size_t g_acc_len; extern FILE *g_acc_file;
    uint8_t *pg = (uint8_t *)(a & ~4095ul); size_t pl = ((a + l + 4095) & ~4095ul) - (uintptr_t)pg;
    g_acc_file = fopen("/home/claude/gp3/out/acc.bin", "wb");
    LOG("watch-addr: %p..+%zx", (void *)pg, pl);
    g_acc_len = pl; g_acc_base = pg; mprotect(pg, pl, PROT_NONE);
    sleep(du);
    g_acc_base = NULL; mprotect(pg, pl, PROT_READ | PROT_WRITE); fflush(g_acc_file);
    LOG("watch-addr: done");
    return NULL;
}
static int g_wide_w, g_wide_h, g_fov = 100;   /* --fov 0: original field of view */
int main(int argc, char **argv) {
    int i = 1;
    g_cfg.dump_every = 30;
    g_cfg.scale = 2;
    for (; i < argc && argv[i][0] == '-'; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--selftest")) { printf("gp3-game: 32-bit execution OK\n"); return 0; }
        else if (!strcmp(a, "--headless")) g_cfg.headless = 1;
        else if (!strcmp(a, "--windowed")) g_cfg.windowed = 1;
        else if (!strcmp(a, "--dump-dir") && i + 1 < argc) snprintf(g_cfg.dump_dir, sizeof g_cfg.dump_dir, "%s", argv[++i]);
        else if (!strcmp(a, "--dump-every") && i + 1 < argc) g_cfg.dump_every = atoi(argv[++i]);
        else if (!strcmp(a, "--max-seconds") && i + 1 < argc) g_cfg.max_seconds = atoi(argv[++i]);
        else if (!strcmp(a, "--script") && i + 1 < argc) snprintf(g_cfg.script, sizeof g_cfg.script, "%s", argv[++i]);
        else if (!strcmp(a, "--log") && i + 1 < argc) { g_log = fopen(argv[++i], "w"); if (g_log) setvbuf(g_log, NULL, _IOLBF, 0); }   /* line by line: nothing lost in a crash */
        else if (!strcmp(a, "--trace-d3d")) g_trace_d3d = 1;
        else if (!strcmp(a, "--inject") && i + 1 < argc) { g_inject = argv[++i]; }
        else if (!strcmp(a, "--native-ddraw")) { extern int g_native_ddraw; g_native_ddraw = 1; }
        else if (!strcmp(a, "--trap-selftest")) g_trap_selftest = 1;
        else if (!strcmp(a, "--spray") && i + 1 < argc) { extern int g_spray_opacity; g_spray_opacity = atoi(argv[++i]); }
        else if (!strcmp(a, "--no-overlay")) { extern int g_overlay_locks; g_overlay_locks = 0; }
        else if (!strcmp(a, "--real-bink")) { extern int g_use_real_bink; g_use_real_bink = 1; }
        else if (!strcmp(a, "--wide") && i + 2 < argc) { g_wide_w = atoi(argv[++i]); g_wide_h = atoi(argv[++i]); }
        else if (!strcmp(a, "--fov") && i + 1 < argc) g_fov = atoi(argv[++i]);
        else if (!strcmp(a, "--scratch-site") && i + 1 < argc) { extern uint32_t g_scratch_site; g_scratch_site = strtoul(argv[++i], NULL, 16); }
        else if (!strcmp(a, "--diff-site") && i + 1 < argc) { extern uint32_t g_diff_site; g_diff_site = strtoul(argv[++i], NULL, 16); }
        else if (!strcmp(a, "--capture") && i + 1 < argc) snprintf(g_capture_path, sizeof g_capture_path, "%s", argv[++i]);
        else if (!strcmp(a, "--capture-from") && i + 1 < argc) { extern double g_capture_from; g_capture_from = atof(argv[++i]); }
        else if (!strcmp(a, "--dump-textures")) { extern int g_dump_textures; g_dump_textures = 1; }
        else if (!strcmp(a, "--probe") && i + 1 < argc) { extern int g_probe_x, g_probe_y; sscanf(argv[++i], "%d,%d", &g_probe_x, &g_probe_y); }
        else if (!strcmp(a, "-v")) g_log_level = 2;
        else if (!strcmp(a, "-vv")) g_log_level = 3;
        else usage();
    }
    if (i >= argc) usage();
    if (!realpath(argv[i], g_game_dir)) rt_fatal("game dir %s not found", argv[i]);
    i++;
    if (!g_cfg.dump_dir[0]) snprintf(g_cfg.dump_dir, sizeof g_cfg.dump_dir, "%s/screens", g_game_dir);

    /* the game executable: GP3 1.13 Gp3.exe, GP3 2000 "gp3 2000.exe" (or a copy named Gp3.exe) */
    long esz = 0;
    if (!gameexe_find(g_game_dir, g_exe_file, sizeof g_exe_file, &esz)) rt_fatal("game executable (Gp3.exe / gp3 2000.exe) not found in %s", g_game_dir);
    /* Windows-style command line: "C:\GP3\Gp3.exe" args... */
    char cmd[2048];
    size_t o = snprintf(cmd, sizeof cmd, "\"C:\\GP3\\%s\"", g_exe_file);
    for (; i < argc; i++) o += snprintf(cmd + o, sizeof cmd - o, " %s", argv[i]);
    win32_set_cmdline(cmd);

    rt_now_us();
    mem_reserve_high();
    win32_register_all();

    char probe;
    uint32_t top = ((uint32_t)(uintptr_t)&probe + 0xfff) & ~0xfffu;
    teb_t *t = teb_create(top, top - 0x800000);
    teb_activate(t);
    rt_install_signal_handlers();
    if (g_trap_selftest) {
        /* push edi; mov edi,[esp+8]; xor ecx,ecx; (mov cl,[edi] patched:) int3; nop; mov eax,ecx; pop edi; ret */
        extern uint32_t g_extra_shadow_site;
        uint8_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        static const uint8_t prog[] = { 0x57, 0x8b, 0x7c, 0x24, 0x08, 0x31, 0xc9, 0xcc, 0x90, 0x89, 0xc8, 0x5f, 0xc3 };
        memcpy(code, prog, sizeof prog);
        g_extra_shadow_site = (uint32_t)(uintptr_t)(code + 7);
        static uint8_t mem[4] = { 0x5a, 0xa5, 0x11, 0x22 };
        uint32_t (*fn)(void *) = (uint32_t (*)(void *))code;
        uint32_t a = fn(mem), b = fn(mem + 1);
        LOG("trap self-test: %02x %02x (expected 5a a5) -> %s", a & 0xff, b & 0xff, (a & 0xff) == 0x5a && (b & 0xff) == 0xa5 ? "OK" : "FAILED");
        g_extra_shadow_site = 0;
    }

    char exe[1024];
    {   /* GP3 1.13: Gp3.exe; GP3 2000: "gp3 2000.exe" (or a copy named Gp3.exe) */
        const char *en = g_exe_file;
        snprintf(exe, sizeof exe, "%s/%s", g_game_dir, en);
        LOG("game executable: %s (%ld bytes)", en, esz);
        if (gameexe_check(exe, NULL) == 1) LOG("WARNING: %s has SafeDisc copy protection, which does not run outside Windows: use an executable without SafeDisc", en);
    }
    module_t *m = pe_load(exe, "gp3.exe", 1);
    if (!m) rt_fatal("could not load %s", exe);
    ((teb_t *)t)->peb && (*(uint32_t *)(uintptr_t)(t->peb + 8) = m->base);

    video_init();
    gp3_addrs_reset();
    if (g_wide_w > 0 && g_wide_h > 0) gp3_widen_modes(g_wide_w, g_wide_h);
    if (g_fov > 0) gp3_hor_plus(g_fov);
    if (!getenv("GP3_NO_EDGEFIX")) gp3_raise_edge_limit();
    /* GPxPatch (when present) */
    int gpx_wanted = 0;
    { char hp[1024];
      int have = path_to_host("gpxpdata\\gxm\\gpxpatch.gxm", hp, sizeof hp) && path_to_host("GPxPSup.dll", hp, sizeof hp);
#ifdef GP3_BRIDGE
      gpx_wanted = have && !getenv("GP3_NO_GPX");
#else
      gpx_wanted = have && getenv("GP3_GPX") != NULL;   /* test harness: opt-in */
#endif
      if (!have && path_to_host("gpxpdata\\gxm\\gpxpatch.gxm", hp, sizeof hp)) LOG("GPxPatch: GPxPSup.dll missing from the game folder: modules not loaded");
    }
    if (!gpx_wanted) gp3_fix_string_table();   /* GPxPatch rewrites the string table loader itself */
    gp3_fix_sound_lock();
    if (!getenv("GP3_NO_FXFIX")) gp3_fix_replay_effects();
    if (!getenv("GP3_NO_REPLAY2H")) gp3_replay_2h();
    { extern void gp3_replay_keys(int legend); if (!getenv("GP3_NO_REPLAYKEYS")) gp3_replay_keys(!getenv("GP3_NO_REPLAYLEGEND")); }
    /* GP3Replay.dll (replay of the whole race, GP3 1.13): the same add-on as on Windows */
    { char hp[1024];
      if (!getenv("GP3_NO_GP3REPLAY") && path_to_host("GP3Replay.dll", hp, sizeof hp)) {
          module_t *vm = pe_load(hp, "GP3Replay.dll", 0);
          uint32_t st = vm ? pe_get_proc(vm, "GP3Replay_Status", 0) : 0;
          static char msg[256]; msg[0] = 0;
          if (st) ((int (__attribute__((stdcall)) *)(char *, int))(uintptr_t)st)(msg, sizeof msg);
          LOG("%s", vm ? (st ? msg : "GP3Replay.dll loaded (no status)") : "GP3Replay.dll found but could not be loaded");
      } }
    { extern void rptrace_install(void) __attribute__((weak)); if (rptrace_install) rptrace_install(); }
    if (gpx_wanted) {
        /* GPxPatch: on Windows its launcher starts GP3 under a debugger and injects code that
           loads gpxpdata\gxm\gpxpatch.gxm at the entry point; that module loads the others
           (GPxSet, GPxLap, GPxTrack, GPxCInfo, GPxSat, GPxSound, GPxIvibe and the GPxPDX7 drawing
           layer) according to the .ini files in gpxpdata. Here it is simply loaded. */
        char hp[1024];
        path_to_host("gpxpdata\\gxm\\gpxpatch.gxm", hp, sizeof hp);
        module_t *g = pe_load(hp, "gpxpatch.gxm", 0);
        { extern void gpx_fixups(void); if (g && !getenv("GP3_GPX_NOFIX")) gpx_fixups(); }
        traps_install();
        LOG("GPxPatch: modules %s (gpxpdata\\gxm\\gpxpatch.gxm; their logs are in gpxpdata\\log)", g ? "loaded" : "could not be loaded");
        gpx_dump_modules();
    }
    if (getenv("GP3_WATCH_ADDR")) { pthread_t th; pthread_create(&th, NULL, watch_addr_thread, NULL); }
    if (getenv("GP3_MEMDUMP")) { pthread_t th; pthread_create(&th, NULL, memdump_thread, NULL); }
    if (getenv("GP3_POLL")) { pthread_t th; pthread_create(&th, NULL, poll_thread, NULL); }
    if (getenv("GP3_WATCH")) { pthread_t th; pthread_create(&th, NULL, watch_thread, NULL); }
    if (getenv("GP3_STUB_SELFTEST")) {
        extern uint8_t *d3d_build_shadow_stub(const uint8_t *addinsn);
        static uint32_t addend = 0x80000001; static uint8_t probe = 0x5a;
        uint8_t ins[6] = { 0x03, 0x05 }; uint32_t aa = (uint32_t)(uintptr_t)&addend; memcpy(ins + 2, &aa, 4);
        uint8_t *stub = d3d_build_shadow_stub(ins);
        /* push edi; push ebx; mov edi,[esp+12]; mov ecx,0x11223300; mov eax,0x80000000; call stub; nop*3; setc bl; movzx ebx,bl; shl ebx,8; mov bl,cl; mov eax,ebx; pop ebx; pop edi; ret */
        uint8_t *c = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); int k = 0;
        uint8_t pro[] = { 0x57, 0x53, 0x8b, 0x7c, 0x24, 0x0c, 0xb9, 0x00, 0x33, 0x22, 0x11, 0xb8, 0x00, 0x00, 0x00, 0x80 };
        memcpy(c, pro, sizeof pro); k = sizeof pro;
        c[k++] = 0xe8; int32_t rel = (int32_t)((uintptr_t)stub - ((uintptr_t)c + k + 4)); memcpy(c + k, &rel, 4); k += 4;
        c[k++] = 0x90; c[k++] = 0x90; c[k++] = 0x90;
        uint8_t epi[] = { 0x0f, 0x92, 0xc3, 0x0f, 0xb6, 0xdb, 0xc1, 0xe3, 0x08, 0x88, 0xcb, 0x89, 0xd8, 0x5b, 0x5f, 0xc3 };
        memcpy(c + k, epi, sizeof epi);
        uint32_t r = ((uint32_t (*)(void *))c)(&probe);
        printf("stub selftest: cl=%02x carry=%u (expect cl=5a carry=1)\n", r & 0xff, (r >> 8) & 1);
        return 0;
    }
    if (getenv("GP3_NOFAULT")) { extern int g_nofault, g_lazy_readback; g_nofault = 1; g_lazy_readback = 0; LOG("testing: exception-free mode"); }
    if (getenv("GP3_FORCE_MODE")) {   /* testing: force the race resolution index (like WidePrix3 does) */
        static int idx; idx = atoi(getenv("GP3_FORCE_MODE"));
        pthread_t th; pthread_create(&th, NULL, force_mode_thread, &idx);
    }
    script_start();
    if (g_inject) {
        typedef HMODULE (WINAPI *ll_t)(const char *);
        ll_t ll = (ll_t)rt_find_export("kernel32.dll", "LoadLibraryA", 0);
        HMODULE h = ll ? ll(g_inject) : 0;
        LOG("inject %s -> %x", g_inject, (unsigned)h);
        if (getenv("GP3_DUMP_MODS")) {
            static const struct { uint32_t base, size; const char *name; } mods[] = { { 0x09900000, 0x1f000, "/home/claude/gp3/out/gpxpsup.mem" }, { 0x09a00000, 0x15000, "/home/claude/gp3/out/gpxpatchgxm.mem" } };
            for (int k = 0; k < 2; k++) { FILE *f = fopen(mods[k].name, "wb"); if (f) { fwrite((void *)(uintptr_t)mods[k].base, 1, mods[k].size, f); fclose(f); } }
        }
    }
    LOG("starting game at 0x%08x, cmdline: %s", m->entry, cmd);
    typedef void (WINAPI *entry_t)(void);
    ((entry_t)(uintptr_t)m->entry)();
    return 0;
}

int plat_mkdir(const char *path) { return mkdir(path, 0755); }
