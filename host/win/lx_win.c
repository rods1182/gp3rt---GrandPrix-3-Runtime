/* GPxPatch GP3RT for Windows - the lx_* functions of host/launcher.h for Windows:
   files and .ini (the same code as the Linux launcher), and starting the game.
   "Start with GPxPatch" runs the original GPxPatch.exe /R (it injects its modules and
   starts the game without its own window); "without" runs the game executable. */
#include "win_compat.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <dirent.h>
#include <tlhelp32.h>
#include "../launcher.h"
#include "../../src/gameexe.h"
void hlog(const char *fmt, ...);
static const char *VERSION = "?";
static char g_dir[PATH_MAX], g_gpxdir[PATH_MAX];
static char *ci_strstr(const char *h, const char *n) {   /* strcasestr without _GNU_SOURCE */
    size_t l = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, l)) return (char *)h;
    return NULL;
}

typedef struct { char *p; size_t n, cap; } sb_t;
static void sb_put(sb_t *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 256; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void sb_puts(sb_t *b, const char *s) { sb_put(b, s, strlen(s)); }

static int is_utf8_text(const char *s, size_t len) {
    int multi = 0;
    for (const unsigned char *p = (const unsigned char *)s, *e = p + len; p < e; ) {
        if (*p < 0x80) { p++; continue; }
        int n = (*p & 0xe0) == 0xc0 ? 2 : (*p & 0xf0) == 0xe0 ? 3 : (*p & 0xf8) == 0xf0 ? 4 : 0;
        if (!n || *p < 0xc2 || p + n > e) return 0;
        for (int i = 1; i < n; i++) if ((p[i] & 0xc0) != 0x80) return 0;
        p += n; multi = 1;
    }
    return multi;
}

static void utf8_to_latin1(char *s) {
    unsigned char *r = (unsigned char *)s, *w = r;
    while (*r) {
        if (*r < 0x80) *w++ = *r++;
        else if ((*r & 0xe0) == 0xc0 && (r[1] & 0xc0) == 0x80) { unsigned v = ((r[0] & 0x1f) << 6) | (r[1] & 0x3f); *w++ = v < 256 ? (unsigned char)v : '?'; r += 2; }
        else { *w++ = '?'; r++; while ((*r & 0xc0) == 0x80) r++; }
    }
    *w = 0;
}

static int find_ci(const char *dir, const char *name, char *out, size_t outsz) {   /* case-insensitive lookup */
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int ok = 0;
    while ((e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, outsz, "%s/%s", dir, e->d_name); ok = 1; break; }
    closedir(d); return ok;
}
static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (8 << 20)) { fclose(f); return NULL; }
    char *p = malloc((size_t)n + 1); size_t r = fread(p, 1, (size_t)n, f); fclose(f);
    p[r] = 0; if (len) *len = r; return p;
}
/* "a\b\c" (Windows separators, any case) under base -> real host path; 1 if it exists */
static int ci_walk(const char *base, const char *rel, char *out, size_t outsz) {
    char cur[PATH_MAX]; snprintf(cur, sizeof cur, "%s", base[0] ? base : "/");
    char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s", rel);
    for (char *q = tmp; *q; q++) if (*q == '\\') *q = '/';
    char *sv = NULL;
    for (char *t = strtok_r(tmp, "/", &sv); t; t = strtok_r(NULL, "/", &sv)) {
        if (!strcmp(t, ".")) continue;
        char nx[PATH_MAX];
        if (!find_ci(cur, t, nx, sizeof nx)) return 0;
        snprintf(cur, sizeof cur, "%s", nx);
    }
    snprintf(out, outsz, "%s", cur); return 1;
}

/* what kind of .set this is: 's' season set ([CarSets]), 'c' car set, 't' track set */
static char set_kind(const char *path) {
    size_t n; char *t = read_file(path, &n); if (!t) return 't';
    char k = 't';
    if (ci_strstr(t, "[CarSets]")) k = 's';
    else if (ci_strstr(t, "[Cars]") || ci_strstr(t, "[Cockpits]") || ci_strstr(t, "[TeamPitOrder]") || ci_strstr(t, "[JackMen]") || ci_strstr(t, "[PitCrews]")) k = 'c';
    free(t); return k;
}

/* which files the page may read and write: gp3rt.ini (game folder) and plain names inside gpxpdata */
static int file_path(const char *name, char *out, size_t outsz) {
    if (!strcasecmp(name, "gp3rt.ini")) { snprintf(out, outsz, "%s/gp3rt.ini", g_dir); return 1; }
    if (!g_gpxdir[0] || !name[0] || name[0] == '.') return 0;
    const char *dot = strrchr(name, '.');
    if (!dot || (strcasecmp(dot, ".ini") && strcasecmp(dot, ".set"))) return 0;
    if (strchr(name, '/') || strchr(name, '\\')) {
        /* GPxCInfo panels: pnl\<folder>\<panel>.ini (no "..", no other folders) */
        if (strncasecmp(name, "pnl\\", 4) && strncasecmp(name, "pnl/", 4)) return 0;
        if (strstr(name, "..") || strcasecmp(dot, ".ini")) return 0;
        if (ci_walk(g_gpxdir, name, out, outsz)) return 1;
        char rel[PATH_MAX]; snprintf(rel, sizeof rel, "%s", name); for (char *c = rel; *c; c++) if (*c == '\\') *c = '/';
        char *sl = strrchr(rel, '/'); *sl = 0;   /* the folder must exist (new panels go into an existing one) */
        char dir[PATH_MAX]; if (!ci_walk(g_gpxdir, rel, dir, sizeof dir)) return 0;
        snprintf(out, outsz, "%s/%s", dir, sl + 1); return 1;
    }
    if (!find_ci(g_gpxdir, name, out, outsz)) snprintf(out, outsz, "%s/%s", g_gpxdir, name);
    return 1;
}

/* set (or delete, val == NULL) a key in an .ini file, keeping everything else as it is */
static int ini_set(const char *path, const char *sec, const char *key, const char *val) {
    size_t len = 0; char *txt = read_file(path, &len);
    if (!txt) { txt = strdup(""); len = 0; }
    const char *nl = strstr(txt, "\r\n") || (!len && !strstr(path, "gp3rt.ini")) ? "\r\n" : "\n";
    sb_t o = { 0 };
    int in = 0, done = 0, seen_sec = 0; size_t sec_end = 0;   /* output offset just after the last line of our section */
    char *p = txt;
    while (*p) {
        char *e = strchr(p, '\n'); size_t ll = e ? (size_t)(e - p + 1) : strlen(p);
        char line[1024]; size_t cl = ll < sizeof line - 1 ? ll : sizeof line - 1; memcpy(line, p, cl); line[cl] = 0;
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') {
            char *b = strchr(s, ']');
            if (b) { *b = 0; int was = in; in = !strcasecmp(s + 1, sec); if (in) seen_sec = 1; if (was && !in && !done) { /* leaving our section */ } }
            sb_put(&o, p, ll);
            if (in) sec_end = o.n;
        } else if (in && !done && *s != ';' && *s != '#' && strchr(s, '=')) {
            char *eq = strchr(s, '='); char k[256]; size_t kl = (size_t)(eq - s); while (kl && (s[kl - 1] == ' ' || s[kl - 1] == '\t')) kl--;
            if (kl >= sizeof k) kl = sizeof k - 1; memcpy(k, s, kl); k[kl] = 0;
            if (!strcasecmp(k, key)) {
                if (val) { sb_puts(&o, k); sb_puts(&o, "="); sb_puts(&o, val); sb_puts(&o, e ? nl : ""); if (!e) sb_puts(&o, nl); }
                done = 1;
            } else sb_put(&o, p, ll);
            if (in) sec_end = o.n;
        } else {
            sb_put(&o, p, ll);
            if (in && *s && *s != '\r' && *s != '\n') sec_end = o.n;
        }
        p += ll;
    }
    if (!done && val) {
        sb_t line = { 0 }; sb_puts(&line, key); sb_puts(&line, "="); sb_puts(&line, val); sb_puts(&line, nl);
        if (seen_sec) {   /* insert at the end of the section */
            if (sec_end > 0 && o.p[sec_end - 1] != '\n') { sb_t t = { 0 }; sb_put(&t, o.p, sec_end); sb_puts(&t, nl); sb_put(&t, line.p, line.n); sb_put(&t, o.p + sec_end, o.n - sec_end); free(o.p); o = t; }
            else { sb_t t = { 0 }; sb_put(&t, o.p ? o.p : "", sec_end); sb_put(&t, line.p, line.n); sb_put(&t, o.p ? o.p + sec_end : "", o.n - sec_end); free(o.p); o = t; }
        } else {
            if (o.n && o.p[o.n - 1] != '\n') sb_puts(&o, nl);
            sb_puts(&o, "["); sb_puts(&o, sec); sb_puts(&o, "]"); sb_puts(&o, nl); sb_put(&o, line.p, line.n);
        }
        free(line.p);
    }
    free(txt);
    if (!done && !val) { free(o.p); return 1; }   /* nothing to delete */
    char tmp[PATH_MAX + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) { free(o.p); return 0; }
    fwrite(o.p ? o.p : "", 1, o.n, f); fclose(f); free(o.p);
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
}


/* a path as GP3 sees it: relative to the game folder, or absolute (C:\...) */
static int game_file(const char *win, char *out, size_t outsz) {
    if (win[0] && win[1] == ':') { char t[PATH_MAX]; snprintf(t, sizeof t, "%s", win); win_slashes(t);
        if (GetFileAttributesA(t) == INVALID_FILE_ATTRIBUTES) return 0; snprintf(out, outsz, "%s", t); return 1; }
    return ci_walk(g_dir, win, out, outsz);
}

/* ------------------------------------------------------------------ */
/* the game                                                             */
static HANDLE g_proc; static int g_last_exit = -1; static DWORD g_check_t; static int g_gpx_launch;
static const char *g_game_exe_names[] = { "gp3.exe", "gp3 2000.exe", "gp32000.exe", "gp3_2000.exe", "gp3-2000.exe", NULL };
static int game_process_alive(void) {   /* the game started by GPxPatch.exe (which exits right after) */
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe; pe.dwSize = sizeof pe; int alive = 0;
    if (Process32First(s, &pe)) do { for (int i = 0; g_game_exe_names[i]; i++) if (!strcasecmp(pe.szExeFile, g_game_exe_names[i])) alive = 1; } while (!alive && Process32Next(s, &pe));
    CloseHandle(s); return alive;
}
static int run(const char *exe, const char *args) {
    char cmd[PATH_MAX * 2]; snprintf(cmd, sizeof cmd, "\"%s\"%s%s", exe, args ? " " : "", args ? args : "");
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", g_dir);
    STARTUPINFOA si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si; memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) { hlog("could not start %s (error %lu)", cmd, GetLastError()); return 0; }
    CloseHandle(pi.hThread); g_proc = pi.hProcess; hlog("started %s", cmd); return 1;
}
void lx_start_game(int with_gpx) {
    char p[PATH_MAX], en[256];
    g_gpx_launch = 0; g_last_exit = -1;
    if (with_gpx && find_ci(g_dir, "GPxPatch.exe", p, sizeof p)) { g_gpx_launch = 1; run(p, "/R"); return; }
    if (with_gpx) hlog("GPxPatch.exe not found: starting the game without GPxPatch");
    if (!gameexe_find(g_dir, en, sizeof en, NULL)) { hlog("game executable not found in %s", g_dir); return; }
    snprintf(p, sizeof p, "%s/%s", g_dir, en); run(p, NULL);
}
int lx_game_running(void) {
    if (g_proc) {
        if (WaitForSingleObject(g_proc, 0) != WAIT_OBJECT_0) return 1;
        DWORD c = 0; GetExitCodeProcess(g_proc, &c); CloseHandle(g_proc); g_proc = 0;
        if (!g_gpx_launch) { g_last_exit = (int)c; return 0; }
        g_check_t = GetTickCount();   /* GPxPatch.exe started the game and left: follow the game itself */
        if (c) { g_last_exit = (int)c; g_gpx_launch = 0; return 0; }
    }
    if (g_gpx_launch) {
        static int seen; static DWORD last; DWORD now = GetTickCount();
        if (now - last < 500) return 1; last = now;
        int alive = game_process_alive(); if (alive) seen = 1;
        if (alive || (!seen && now - g_check_t < 15000)) return 1;
        g_gpx_launch = 0; seen = 0; g_last_exit = 0;
    }
    return 0;
}
int lx_last_exit(void) { return g_last_exit; }

/* ------------------------------------------------------------------ */
void lx_setup(const char *game_dir, const char *version) {
    VERSION = version; snprintf(g_dir, sizeof g_dir, "%s", game_dir); win_slashes(g_dir);
    if (!find_ci(g_dir, "gpxpdata", g_gpxdir, sizeof g_gpxdir)) g_gpxdir[0] = 0;
}
const char *lx_dir(void) { return g_dir; }
const char *lx_gpxdir(void) { return g_gpxdir; }
const char *lx_version(void) { return VERSION; }
const char *lx_exe_version(void) { char en[256]; long sz = 0; if (!gameexe_find(g_dir, en, sizeof en, &sz)) return "?"; return sz > 9000000 ? "2000" : "1.13"; }
int lx_find_ci(const char *dir, const char *name, char *out, size_t outsz) { return find_ci(dir, name, out, outsz); }
int lx_ci_walk(const char *base, const char *rel, char *out, size_t outsz) { return ci_walk(base, rel, out, outsz); }
int lx_game_file(const char *win, char *out, size_t outsz) { return game_file(win, out, outsz); }
char *lx_read_file(const char *path, size_t *len) { return read_file(path, len); }
int lx_file_path(const char *name, char *out, size_t outsz) { return file_path(name, out, outsz); }
int lx_ini_set(const char *path, const char *sec, const char *key, const char *val) { return ini_set(path, sec, key, val); }
char lx_set_kind(const char *path) { return set_kind(path); }
int lx_is_utf8(const char *s, size_t n) { return is_utf8_text(s, n); }
void lx_utf8_to_latin1(char *s) { utf8_to_latin1(s); }
int lx_make_shortcuts(void) { return 0; }
int lx_bundled_gpx(void) { return 0; }
int lx_install_bundled_gpx(void) { return -1; }
int lx_shortcuts_exist(void) { return 1; }
void lx_folder_launchers(void) {}
