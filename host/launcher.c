/* GP3RT for Linux - GPxPatch launcher / configuration screen.
 *
 * "gp3 --gpxpatch [game-folder]" starts a small web server on 127.0.0.1 and opens the
 * configuration page (host/launcher_ui.html, compiled in) in the user's browser. The page
 * reads and writes the same .ini files the Windows GPxPatch writes (gpxpdata\*.ini,
 * keys.ini, fonts.ini, the track set) plus gp3rt.ini, and starts the game with or without
 * GPxPatch. Nothing to install: every desktop already has a browser.
 *
 * Safety: the server only listens on 127.0.0.1, every API call needs the random token
 * that is in the URL handed to the browser, the Host header must be the loopback address
 * (no DNS rebinding) and only known .ini files inside the game folder can be written.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "launcher_ui.h"   /* generated: static const char g_launcher_html[] */
#include "launcher.h"

void hlog(const char *fmt, ...);
static const char *VERSION = "?";
static char *ci_strstr(const char *h, const char *n) {   /* strcasestr without _GNU_SOURCE */
    size_t l = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, l)) return (char *)h;
    return NULL;
}
#define strcasestr ci_strstr

static char g_dir[PATH_MAX];       /* game folder */
static char g_gpxdir[PATH_MAX];    /* its gpxpdata folder ("" when GPxPatch is not installed) */
static char g_token[40];
static pid_t g_game = 0;
static int g_last_exit = -1;
static time_t g_last_seen, g_bye;   /* g_bye: the page said it is closing */

/* ------------------------------------------------------------------ */
/* small string buffer                                                  */
typedef struct { char *p; size_t n, cap; } sb_t;
static void sb_put(sb_t *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 256; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void sb_puts(sb_t *b, const char *s) { sb_put(b, s, strlen(s)); }
static void sb_printf(sb_t *b, const char *fmt, ...) {
    char tmp[4096]; va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n > 0) sb_put(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}
/* JSON string from Latin-1 (the GPx files are Windows-1252/Latin-1) */
static void sb_json_latin1(sb_t *b, const char *s, size_t n) {
    sb_puts(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { char t[2] = { '\\', (char)c }; sb_put(b, t, 2); }
        else if (c == '\n') sb_puts(b, "\\n");
        else if (c == '\r') sb_puts(b, "\\r");
        else if (c == '\t') sb_puts(b, "\\t");
        else if (c < 0x20) sb_printf(b, "\\u%04x", c);
        else if (c < 0x80) sb_put(b, (const char *)&c, 1);
        else { char t[2] = { (char)(0xc0 | (c >> 6)), (char)(0x80 | (c & 0x3f)) }; sb_put(b, t, 2); }
    }
    sb_puts(b, "\"");
}
static void sb_json(sb_t *b, const char *s) { sb_json_latin1(b, s, strlen(s)); }
/* valid UTF-8 with at least one multi-byte character: a file saved as UTF-8 instead of Windows-1252 */
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
static void sb_json_utf8n(sb_t *b, const char *s, size_t n);
/* file text for the page: Windows-1252 (the normal case) or UTF-8 as is */
static void sb_json_text(sb_t *b, const char *s, size_t n) { if (is_utf8_text(s, n)) sb_json_utf8n(b, s, n); else sb_json_latin1(b, s, n); }
/* JSON string from UTF-8 (file names on Linux) */
static void sb_json_utf8(sb_t *b, const char *s) {
    sb_puts(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char t[2] = { '\\', (char)c }; sb_put(b, t, 2); }
        else if (c < 0x20) sb_printf(b, "\\u%04x", c);
        else sb_put(b, (const char *)&c, 1);
    }
    sb_puts(b, "\"");
}
static void sb_json_utf8n(sb_t *b, const char *s, size_t n) {
    sb_puts(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { char t[2] = { '\\', (char)c }; sb_put(b, t, 2); }
        else if (c == '\n') sb_puts(b, "\\n");
        else if (c == '\r') sb_puts(b, "\\r");
        else if (c == '\t') sb_puts(b, "\\t");
        else if (c < 0x20) sb_printf(b, "\\u%04x", c);
        else sb_put(b, (const char *)&c, 1);
    }
    sb_puts(b, "\"");
}
/* UTF-8 (from the browser) to Latin-1 */
static void utf8_to_latin1(char *s) {
    unsigned char *r = (unsigned char *)s, *w = r;
    while (*r) {
        if (*r < 0x80) *w++ = *r++;
        else if ((*r & 0xe0) == 0xc0 && (r[1] & 0xc0) == 0x80) { unsigned v = ((r[0] & 0x1f) << 6) | (r[1] & 0x3f); *w++ = v < 256 ? (unsigned char)v : '?'; r += 2; }
        else { *w++ = '?'; r++; while ((*r & 0xc0) == 0x80) r++; }
    }
    *w = 0;
}

/* ------------------------------------------------------------------ */
/* files                                                                */
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
/* a path as GP3 sees it (relative to the game folder, or Z:\...) */
static int game_file(const char *win, char *out, size_t outsz) {
    if ((win[0] == 'z' || win[0] == 'Z') && win[1] == ':') return ci_walk("/", win + 2, out, outsz);
    return ci_walk(g_dir, win, out, outsz);
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
        int pnl = !strncasecmp(name, "pnl\\", 4) || !strncasecmp(name, "pnl/", 4);
        if (strstr(name, "..") || name[0] == '/' || name[0] == '\\') return 0;
        if (!pnl && strcasecmp(dot, ".set")) return 0;            /* .set files may sit in sub folders of gpxpdata */
        if (pnl && strcasecmp(dot, ".ini")) return 0;
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
    return rename(tmp, path) == 0;
}

/* ------------------------------------------------------------------ */
/* listings for the page                                                */
static void list_files(sb_t *j, const char *base, const char *rel, const char *ext, int depth, int *count) {
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s%s%s", base, rel[0] ? "/" : "", rel);
    DIR *d = opendir(path); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *count < 600) {
        if (e->d_name[0] == '.') continue;
        char r2[PATH_MAX]; snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        char full[PATH_MAX * 2]; snprintf(full, sizeof full, "%s/%s", base, r2);
        DIR *sub = opendir(full);   /* (no stat(): it needs glibc 2.33) */
        if (sub) {
            closedir(sub);
            if (depth > 0 && strcasecmp(e->d_name, "gpxpdata") && strcasecmp(e->d_name, "screens") && strcasecmp(e->d_name, "gpsaves")) list_files(j, base, r2, ext, depth - 1, count);
        } else {
            const char *dot = strrchr(e->d_name, '.');
            if (dot && !strcasecmp(dot, ext)) {
                for (char *q = r2; *q; q++) if (*q == '/') *q = '\\';
                if ((*count)++) sb_puts(j, ",");
                sb_json_utf8(j, r2);
            }
        }
    }
    closedir(d);
}
#include "../src/gameexe.h"
static const char *exe_version(void) {
    char en[256]; long sz = 0;
    if (!gameexe_find(g_dir, en, sizeof en, &sz)) return "?";
    return sz > 9000000 ? "2000" : "1.13";
}
static int ext_ok(const char *dot, const char *list) {   /* list: ".cs3" or ".ini,.txt" */
    char l[128]; snprintf(l, sizeof l, "%s", list && *list ? list : ".dat");
    for (char *t = strtok(l, ","); t; t = strtok(NULL, ",")) if (!strcasecmp(dot, t)) return 1;
    return 0;
}
static int cmp_str(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }
static void url_decode(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '%' && r[1] && r[2]) { char h[3] = { r[1], r[2], 0 }; *w++ = (char)strtol(h, NULL, 16); r += 2; }
        else *w++ = *r == '+' ? ' ' : *r;
    }
    *w = 0;
}
/* folder browser for the track selection: sub-folders and .dat files of one folder */
static void api_ls(sb_t *j, const char *path, const char *ext) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s", path[0] == '/' ? path : g_dir);
    char real[PATH_MAX], ci[PATH_MAX];
    if (!realpath(p, real)) { if (ci_walk("/", p, ci, sizeof ci) && realpath(ci, real)) {} else snprintf(real, sizeof real, "%s", g_dir); }
    DIR *d = opendir(real); if (!d) { snprintf(real, sizeof real, "%s", g_dir); d = opendir(real); }
    char *dirs[2000], *files[2000]; int nd = 0, nf = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[PATH_MAX * 2]; snprintf(full, sizeof full, "%s/%s", real, e->d_name);
        DIR *sub = opendir(full);
        if (sub) { closedir(sub); if (nd < 2000) dirs[nd++] = strdup(e->d_name); }
        else { const char *dot = strrchr(e->d_name, '.'); if (dot && ext_ok(dot, ext) && nf < 2000) files[nf++] = strdup(e->d_name); }
    }
    if (d) closedir(d);
    qsort(dirs, nd, sizeof *dirs, cmp_str); qsort(files, nf, sizeof *files, cmp_str);
    sb_puts(j, "{\"path\":"); sb_json_utf8(j, real);
    sb_puts(j, ",\"game\":"); sb_json_utf8(j, g_dir);
    sb_puts(j, ",\"home\":"); sb_json_utf8(j, getenv("HOME") ? getenv("HOME") : "/");
    sb_puts(j, ",\"dirs\":["); for (int i = 0; i < nd; i++) { if (i) sb_puts(j, ","); sb_json_utf8(j, dirs[i]); free(dirs[i]); }
    sb_puts(j, "],\"files\":["); for (int i = 0; i < nf; i++) { if (i) sb_puts(j, ","); sb_json_utf8(j, files[i]); free(files[i]); }
    sb_puts(j, "]}");
}
static void api_state(sb_t *j) {
    sb_puts(j, "{\"version\":"); sb_json(j, VERSION);
    sb_puts(j, ",\"dir\":"); sb_json_utf8(j, g_dir);
    sb_printf(j, ",\"exe\":\"%s\",\"gpx\":%d,\"running\":%d", exe_version(), g_gpxdir[0] != 0, g_game > 0);
    { char p[PATH_MAX]; sb_printf(j, ",\"gpxsup\":%d", find_ci(g_dir, "GPxPSup.dll", p, sizeof p)); }
    sb_puts(j, ",\"files\":{");
    static const char *files[] = { "gp3rt.ini", "gpxpatch.ini", "gpxset.ini", "gpxtrack.ini", "gpxlap.ini", "gpxsat.ini",
                                   "gpxcinfo.ini", "gpxsound.ini", "keys.ini", "fonts.ini", NULL };
    int first = 1;
    for (int i = 0; files[i]; i++) {
        char p[PATH_MAX]; if (!file_path(files[i], p, sizeof p)) continue;
        size_t n; char *t = read_file(p, &n);
        sb_printf(j, "%s\"%s\":", first ? "" : ",", files[i]); first = 0;
        if (t) { sb_json_text(j, t, n); free(t); } else sb_puts(j, "null");
    }
    /* the track set named in gpxtrack.ini (default tracks.set) */
    {
        char p[PATH_MAX], set[256] = "tracks.set";
        if (file_path("gpxtrack.ini", p, sizeof p)) {
            size_t n; char *t = read_file(p, &n);
            if (t) { char *k = strcasestr(t, "\nTrackSet="); if (k) { k += 10; size_t l = strcspn(k, "\r\n"); if (l && l < sizeof set) { memcpy(set, k, l); set[l] = 0; } } free(t); }
        }
        if (file_path(set, p, sizeof p)) {
            size_t n; char *t = read_file(p, &n);
            sb_puts(j, ",\"trackset\":"); sb_json(j, set);
            sb_puts(j, ",\"trackset_text\":"); if (t) { sb_json_text(j, t, n); free(t); } else sb_puts(j, "null");
        }
    }
    sb_puts(j, "}");
    {   /* .ini files directly in circuits: GPx uses a lone .ini there for every modified track of the folder */
        char cp[PATH_MAX]; int n = 0;
        sb_puts(j, ",\"circuit_inis\":[");
        if (find_ci(g_dir, "circuits", cp, sizeof cp)) { DIR *d = opendir(cp); struct dirent *e;
            if (d) { while ((e = readdir(d))) { const char *dot = strrchr(e->d_name, '.'); if (dot && !strcasecmp(dot, ".ini")) { if (n++) sb_puts(j, ","); sb_json_utf8(j, e->d_name); } } closedir(d); } }
        sb_puts(j, "]");
    }
    {   /* sky prefixes (skyaNN.bmp -> skya) and tarmac bitmaps in gp3jamsh\techjam */
        char jh[PATH_MAX], tj[PATH_MAX]; int ns = 0, nt = 0; char seen[64][32]; int nseen = 0;
        sb_puts(j, ",\"skies\":["); sb_t t = { 0 };
        if (find_ci(g_dir, "gp3jamsh", jh, sizeof jh) && find_ci(jh, "techjam", tj, sizeof tj)) {
            DIR *d = opendir(tj); struct dirent *e;
            while (d && (e = readdir(d))) {
                size_t l = strlen(e->d_name); const char *n = e->d_name;
                if (l < 8 || strcasecmp(n + l - 4, ".bmp")) continue;
                if (!strncasecmp(n, "sky", 3) && l >= 9 && n[l - 5] >= '0' && n[l - 5] <= '9' && n[l - 6] >= '0' && n[l - 6] <= '9') {
                    char pre[32]; size_t pl = l - 6 < sizeof pre - 1 ? l - 6 : sizeof pre - 1; memcpy(pre, n, pl); pre[pl] = 0;
                    for (char *c = pre; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
                    int dup = 0; for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], pre)) dup = 1;
                    if (!dup && nseen < 64) { strcpy(seen[nseen++], pre); if (ns++) sb_puts(j, ","); sb_json_utf8(j, pre); }
                } else if (strcasestr(n, "tarmac")) { if (nt++) sb_puts(&t, ","); sb_json_utf8(&t, n); }
            }
            if (d) closedir(d);
        }
        sb_puts(j, "],\"tarmacs\":["); if (t.p) sb_puts(j, t.p); sb_puts(j, "]"); free(t.p);
    }
    if (g_gpxdir[0]) {   /* .set files of gpxpdata: track sets, season sets (GP3 2000) and car sets */
        sb_t k[3] = { { 0 }, { 0 }, { 0 } }; int n[3] = { 0, 0, 0 };
        DIR *d = opendir(g_gpxdir); struct dirent *e;
        while (d && (e = readdir(d))) { const char *dot = strrchr(e->d_name, '.');
            if (!dot || strcasecmp(dot, ".set")) continue;
            char p[PATH_MAX * 2]; snprintf(p, sizeof p, "%s/%s", g_gpxdir, e->d_name);
            char c = set_kind(p); int i = c == 's' ? 1 : c == 'c' ? 2 : 0;
            if (n[i]++) sb_puts(&k[i], ","); sb_json_utf8(&k[i], e->d_name); }
        if (d) closedir(d);
        static const char *nm[3] = { "sets", "seasons", "carsets" };
        for (int i = 0; i < 3; i++) { sb_printf(j, ",\"%s\":[", nm[i]); if (k[i].p) sb_puts(j, k[i].p); sb_puts(j, "]"); free(k[i].p); }
    }
    { char p[PATH_MAX]; const char *h = getenv("HOME"); snprintf(p, sizeof p, "%s/.local/share/applications/%s.desktop", h ? h : "/tmp", !strcmp(exe_version(), "2000") ? "gp3-2000-gpxpatch" : "gp3-gpxpatch");
      sb_printf(j, ",\"shortcuts\":%d", access(p, F_OK) == 0); }
    int c = 0;
    sb_puts(j, ",\"dats\":["); list_files(j, g_dir, "", ".dat", 4, &c); sb_puts(j, "]");
    if (g_gpxdir[0]) {
        c = 0; sb_puts(j, ",\"wavs\":["); list_files(j, g_gpxdir, "snd", ".wav", 1, &c); sb_puts(j, "]");
        c = 0; sb_puts(j, ",\"panels\":["); list_files(j, g_gpxdir, "pnl", ".ini", 2, &c); sb_puts(j, "]");
        c = 0; sb_puts(j, ",\"ttfs\":["); list_files(j, g_gpxdir, "gfx/fonts", ".ttf", 1, &c); sb_puts(j, "]");
    }
    sb_puts(j, "}");
}

/* ------------------------------------------------------------------ */
/* the game                                                             */
static void start_game(int with_gpx) {
    if (g_game > 0) return;
    char self[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return;
    self[n] = 0;
    {   /* the game is started by gp3 (GP3RT for Linux), next to this program or in the game folder */
        char *sl = strrchr(self, '/');
        if (sl && strcmp(sl + 1, "gp3")) {
            strcpy(sl + 1, "gp3");
            if (access(self, X_OK)) snprintf(self, sizeof self, "%s/gp3", g_dir);
            if (access(self, X_OK)) { hlog("launcher: gp3 (GP3RT for Linux) not found next to GPxPatch or in the game folder"); return; }
        }
    }
    pid_t p = fork();
    if (p == 0) {
        setsid();
        if (with_gpx) { setenv("GP3_FORCE_GPX", "1", 1); unsetenv("GP3_NO_GPX"); }
        else { setenv("GP3_NO_GPX", "1", 1); unsetenv("GP3_FORCE_GPX"); }
        int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execl(self, self, g_dir, (char *)NULL);
        _exit(127);
    }
    if (p > 0) { g_game = p; g_last_exit = -1; hlog("launcher: game started (pid %d, GPxPatch %s)", (int)p, with_gpx ? "on" : "off"); }
}
static void reap(void) {
    int st;
    if (g_game > 0 && waitpid(g_game, &st, WNOHANG) == g_game) {
        g_last_exit = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        hlog("launcher: game ended (%d)", g_last_exit);
        g_game = 0; g_last_seen = time(NULL);   /* give the page time to notice */
    }
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                 */
static void send_all(int fd, const char *p, size_t n) {
    while (n) { ssize_t w = send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) { if (errno == EINTR) continue; return; } p += w; n -= (size_t)w; }
}
static void reply(int fd, int code, const char *type, const char *body, size_t n) {
    char h[512];
    int hl = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                      "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", code, code == 200 ? "OK" : "Error", type, n);
    send_all(fd, h, (size_t)hl); send_all(fd, body, n);
}
/* menu entries and desktop icons, so nobody needs the terminal */
int extract_icon(const char *exe, const char *out);
/* the program a shortcut runs: GPxPatch (this program) for the screen, gp3 (GP3RT for Linux,
   next to this program or in the game folder) for the game */
static int shortcut_prog(int game, char *out, size_t outsz) {
    ssize_t n = readlink("/proc/self/exe", out, outsz - 1); if (n <= 0) return 0; out[n] = 0;
    if (!game) return 1;
    char *sl = strrchr(out, '/'); if (!sl || !strcmp(sl + 1, "gp3")) return 1;
    strcpy(sl + 1, "gp3");
    if (access(out, X_OK)) snprintf(out, outsz, "%s/gp3", g_dir);
    return access(out, X_OK) == 0;
}
static void desktop_file(const char *path, const char *name, const char *comment, const char *exec_args, const char *icon) {
    char self[PATH_MAX]; if (!shortcut_prog(!*exec_args, self, sizeof self)) return;
    FILE *f = fopen(path, "w"); if (!f) return;
    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nComment=%s\nExec=\"%s\" %s\"%s\"\nPath=%s\nIcon=%s\nTerminal=false\nCategories=Game;Simulation;\n",
            name, comment, self, exec_args, g_dir, g_dir, icon);
    fclose(f); chmod(path, 0755);
    pid_t p = fork();   /* GNOME/Nemo: mark the desktop icon as trusted */
    if (p == 0) { int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); } execlp("gio", "gio", "set", path, "metadata::trusted", "true", (char *)NULL); _exit(127); }
    if (p > 0) waitpid(p, NULL, 0);
}
/* the GP3RT icon as a file path (theme names need the icon cache to be refreshed first) */
static const char *gp3rt_icon(void) {
    static char p[PATH_MAX]; const char *h = getenv("HOME");
    extern int install_icons(void); install_icons();
    snprintf(p, sizeof p, "%s/.local/share/icons/hicolor/256x256/apps/gp3rt.png", h ? h : "/tmp");
    return p;
}
static const char *gpxp_icon(void) {
    static char p[PATH_MAX]; const char *h = getenv("HOME");
    extern int install_icons(void); install_icons();
    snprintf(p, sizeof p, "%s/.local/share/icons/hicolor/256x256/apps/gpxpatch-linux.png", h ? h : "/tmp");
    return p;
}
/* launchers inside the game folder: file managers show them with our icon (a program file
   like gp3 or the GPxPatch link always gets the generic "executable" icon) */
static void folder_launchers(void) {
    const char *ver = exe_version(); int is2000 = !strcmp(ver, "2000");
    const char *gname = is2000 ? "Grand Prix 3 2000" : "Grand Prix 3";
    char p[PATH_MAX * 2], xname[64]; snprintf(xname, sizeof xname, "GPxPatch (%s)", gname);
    const char *icon = gp3rt_icon();
    snprintf(p, sizeof p, "%s/%s.desktop", g_dir, gname); desktop_file(p, gname, "Grand Prix 3 (GP3RT)", "", icon);
    if (g_gpxdir[0]) { snprintf(p, sizeof p, "%s/GPxPatch (%s).desktop", g_dir, gname); desktop_file(p, xname, "GPxPatch", "--gpxpatch ", gpxp_icon()); }
}
void lx_folder_launchers(void) { folder_launchers(); }
static int make_shortcuts(void) {
    const char *h = getenv("HOME"); if (!h) return 0;
    char apps[PATH_MAX], icons[PATH_MAX], p[PATH_MAX * 2];
    snprintf(apps, sizeof apps, "%s/.local/share/applications", h);
    snprintf(icons, sizeof icons, "%s/.local/share/icons/hicolor/32x32/apps", h);
    char tmp[PATH_MAX * 2];
    snprintf(tmp, sizeof tmp, "%s/.local", h); mkdir(tmp, 0755);
    snprintf(tmp, sizeof tmp, "%s/.local/share", h); mkdir(tmp, 0755); mkdir(apps, 0755);
    snprintf(tmp, sizeof tmp, "%s/.local/share/icons", h); mkdir(tmp, 0755);
    snprintf(tmp, sizeof tmp, "%s/.local/share/icons/hicolor", h); mkdir(tmp, 0755);
    snprintf(tmp, sizeof tmp, "%s/.local/share/icons/hicolor/32x32", h); mkdir(tmp, 0755); mkdir(icons, 0755);
    const char *ver = exe_version();
    int is2000 = !strcmp(ver, "2000");
    const char *gid = is2000 ? "grand-prix-3-2000" : "grand-prix-3", *xid = is2000 ? "gp3-2000-gpxpatch" : "gp3-gpxpatch";
    const char *gname = is2000 ? "Grand Prix 3 2000" : "Grand Prix 3";
    char xname[64]; snprintf(xname, sizeof xname, "GPxPatch (%s)", gname);
    extern int install_icons(void); install_icons();   /* GP3RT icon (gp3rt) in every hicolor size */
    { extern void set_folder_icons(const char *); char self[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
      if (n > 0) { self[n] = 0; char *sl = strrchr(self, '/'); if (sl) *sl = 0; set_folder_icons(self); } }
    char gx[PATH_MAX]; int have_gx = find_ci(g_dir, "GPxPatch.exe", gx, sizeof gx);
    if (have_gx) { snprintf(p, sizeof p, "%s/%s.png", icons, xid); if (!extract_icon(gx, p)) have_gx = 0; }
    /* menu */
    const char *icon = gp3rt_icon(); (void)have_gx;
    snprintf(p, sizeof p, "%s/%s.desktop", apps, gid); desktop_file(p, gname, "Grand Prix 3 (GP3RT)", "", icon);
    if (g_gpxdir[0]) { snprintf(p, sizeof p, "%s/%s.desktop", apps, xid); desktop_file(p, xname, "GPxPatch", "--gpxpatch ", gpxp_icon()); }
    folder_launchers();
    /* desktop */
    char desk[PATH_MAX] = "";
    FILE *u = popen("xdg-user-dir DESKTOP 2>/dev/null", "r");
    if (u) { if (fgets(desk, sizeof desk, u)) desk[strcspn(desk, "\r\n")] = 0; pclose(u); }
    if (!desk[0] || !strcmp(desk, h)) { snprintf(desk, sizeof desk, "%s/Desktop", h); DIR *d = opendir(desk); if (d) closedir(d); else snprintf(desk, sizeof desk, "%s/\xc3\x81rea de trabalho", h); }
    DIR *d = opendir(desk);
    if (d) { closedir(d);
        snprintf(p, sizeof p, "%s/%s.desktop", desk, gid); desktop_file(p, gname, "Grand Prix 3 (GP3RT)", "", icon);
        if (g_gpxdir[0]) { snprintf(p, sizeof p, "%s/%s.desktop", desk, xid); desktop_file(p, xname, "GPxPatch", "--gpxpatch ", gpxp_icon()); } }
    pid_t q = fork();
    if (q == 0) { int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); } execlp("update-desktop-database", "update-desktop-database", apps, (char *)NULL); _exit(127); }
    if (q > 0) waitpid(q, NULL, 0);
    hlog("launcher: shortcuts created (menu%s)", d ? " and desktop" : "");
    return 1;
}
static int g_port;
static void handle(int fd) {
    struct timeval tv = { 5, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sb_t req = { 0 }; char buf[8192]; size_t hdr_end = 0, clen = 0;
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r <= 0) break;
        sb_put(&req, buf, (size_t)r);
        if (!hdr_end) { char *e = strstr(req.p, "\r\n\r\n"); if (e) { hdr_end = (size_t)(e - req.p) + 4; char *cl = strcasestr(req.p, "\r\nContent-Length:"); clen = cl ? strtoul(cl + 17, NULL, 10) : 0; } }
        if (hdr_end && req.n >= hdr_end + clen) break;
        if (req.n > (4 << 20)) break;
    }
    if (!hdr_end) { free(req.p); return; }
    req.p[hdr_end - 4] = 0;
    char method[8] = "", path[1024] = "";
    sscanf(req.p, "%7s %1023s", method, path);
    /* Host must be the loopback address (no DNS rebinding) */
    char *host = strcasestr(req.p, "\r\nHost:"); char want[64]; snprintf(want, sizeof want, "127.0.0.1:%d", g_port);
    if (!host || strncmp(host + 7 + strspn(host + 7, " "), want, strlen(want))) { reply(fd, 403, "text/plain", "forbidden", 9); free(req.p); return; }
    g_last_seen = time(NULL);
    if (!strcmp(method, "GET") && (!strcmp(path, "/") || !strncmp(path, "/?", 2))) {
        g_bye = 0;
        reply(fd, 200, "text/html; charset=utf-8", g_launcher_html, strlen(g_launcher_html)); free(req.p); return;
    }
    if (strncmp(path, "/api/", 5)) { reply(fd, 404, "text/plain", "not found", 9); free(req.p); return; }
    if (!strncmp(path, "/api/bye?t=", 11)) {   /* sendBeacon from the closing page (no custom headers possible) */
        if (!strncmp(path + 11, g_token, strlen(g_token))) g_bye = time(NULL);
        reply(fd, 200, "text/plain", "", 0); free(req.p); return;
    }
    g_bye = 0;   /* any other request: the page (or a reloaded one) is alive */
    char *tok = strcasestr(req.p, "\r\nX-Token:");
    if (!tok || strncmp(tok + 10 + strspn(tok + 10, " "), g_token, strlen(g_token))) { reply(fd, 403, "text/plain", "token", 5); free(req.p); return; }
    char *body = req.p + hdr_end; body[clen] = 0;
    sb_t out = { 0 };
    if (!strcmp(path, "/api/state")) { api_state(&out); reply(fd, 200, "application/json", out.p, out.n); }
    else if (!strncmp(path, "/api/ls", 7)) {
        char arg[PATH_MAX] = "", ext[128] = "";
        char *q = strstr(path, "?p="); if (!q) q = strstr(path, "&p=");
        char *x = strstr(path, "&e="); if (!x) x = strstr(path, "?e=");
        if (x) { snprintf(ext, sizeof ext, "%s", x + 3); char *a = strchr(ext, '&'); if (a) *a = 0; url_decode(ext); }
        if (q) { snprintf(arg, sizeof arg, "%s", q + 3); char *a = strchr(arg, '&'); if (a) *a = 0; url_decode(arg); }
        api_ls(&out, arg, ext); reply(fd, 200, "application/json", out.p, out.n);
    }
    else if (!strncmp(path, "/api/file?n=", 12)) {   /* one .set/.ini of gpxpdata (season and car sets) */
        char n[256]; snprintf(n, sizeof n, "%s", path + 12); url_decode(n);
        char p[PATH_MAX]; size_t len = 0; char *t = file_path(n, p, sizeof p) ? read_file(p, &len) : NULL;
        sb_puts(&out, "{\"text\":"); if (t) sb_json_text(&out, t, len); else sb_puts(&out, "null");
        sb_printf(&out, ",\"utf8\":%d}", t ? is_utf8_text(t, len) : 0); free(t);
        reply(fd, 200, "application/json", out.p, out.n);
    }
    else if (!strncmp(path, "/api/gametext?p=", 16)) {   /* a performance .txt (team and driver names for the carset editor) */
        char n[PATH_MAX]; snprintf(n, sizeof n, "%s", path + 16); url_decode(n);
        char hp[PATH_MAX], l1[PATH_MAX]; snprintf(l1, sizeof l1, "%s", n); utf8_to_latin1(l1);
        const char *dot = strrchr(n, '.'); size_t len = 0; char *t = NULL;
        if (dot && (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".ini")) && (game_file(n, hp, sizeof hp) || game_file(l1, hp, sizeof hp))) t = read_file(hp, &len);
        if (t && len < (1 << 20)) sb_json_text(&out, t, len); else sb_puts(&out, "null");
        free(t); reply(fd, 200, "application/json", out.p, out.n);
    }
    else if (!strcmp(path, "/api/exists") && !strcmp(method, "POST")) {
        /* one path per line, alternatives separated by '|': [1,0,...] (does any of them exist) */
        sb_puts(&out, "[");
        int i = 0;
        for (char *line = body; line && *line; i++) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            int ok = 0;
            for (char *a = line; a && !ok; ) {
                char *bar = strchr(a, '|'); if (bar) *bar = 0;
                char hp[PATH_MAX], l1[PATH_MAX]; snprintf(l1, sizeof l1, "%s", a); utf8_to_latin1(l1);
                ok = a[0] && (game_file(a, hp, sizeof hp) || game_file(l1, hp, sizeof hp));
                a = bar ? bar + 1 : NULL;
            }
            sb_printf(&out, "%s%d", i ? "," : "", ok);
            line = nl ? nl + 1 : NULL;
        }
        sb_puts(&out, "]"); reply(fd, 200, "application/json", out.p, out.n);
    }
    else if (!strcmp(path, "/api/status")) {
        reap(); sb_printf(&out, "{\"running\":%d,\"last_exit\":%d}", g_game > 0, g_last_exit); reply(fd, 200, "application/json", out.p, out.n);
    } else if (!strcmp(path, "/api/save") && !strcmp(method, "POST")) {
        /* one change per line: file \t section \t key \t value  (value \x7f = delete the key) */
        int ok = 0, bad = 0;
        for (char *line = strtok(body, "\n"); line; line = strtok(NULL, "\n")) {
            char *f[4] = { line, NULL, NULL, NULL }; int k = 1;
            for (char *q = line; *q && k < 4; q++) if (*q == '\t') { *q = 0; f[k++] = q + 1; }
            if (k < 4) { bad++; continue; }
            char p[PATH_MAX];
            if (!file_path(f[0], p, sizeof p) || !f[1][0] || !f[2][0]) { bad++; continue; }
            { size_t n = 0; char *t = read_file(p, &n); int u8 = t && is_utf8_text(t, n); free(t);
              if (!u8) { utf8_to_latin1(f[3]); utf8_to_latin1(f[2]); utf8_to_latin1(f[1]); } }   /* GPx files are Windows-1252; keep a UTF-8 one UTF-8 */
            for (int i = 1; i < 4; i++) if (strpbrk(f[i], "\r\n") || (i < 3 && (strchr(f[i], '=') || strchr(f[i], ']')))) bad++;
            if (ini_set(p, f[1], f[2], strcmp(f[3], "\x7f") ? f[3] : NULL)) ok++; else bad++;
        }
        sb_printf(&out, "{\"ok\":%d,\"failed\":%d}", ok, bad); reply(fd, 200, "application/json", out.p, out.n);
    } else if (!strcmp(path, "/api/shortcuts") && !strcmp(method, "POST")) {
        int r = make_shortcuts(); sb_printf(&out, "{\"ok\":%d}", r); reply(fd, 200, "application/json", out.p, out.n);
    } else if (!strcmp(path, "/api/copyset") && !strcmp(method, "POST")) {
        /* body: from \t to (both plain .set names inside gpxpdata) */
        char *tab = strchr(body, '\t'); int ok = 0;
        if (tab) {
            *tab = 0; char *to = tab + 1; to[strcspn(to, "\r\n")] = 0;
            const char *dot = strrchr(to, '.');
            char a[PATH_MAX], b[PATH_MAX];
            if (dot && !strcasecmp(dot, ".set") && file_path(body, a, sizeof a) && file_path(to, b, sizeof b) && access(b, F_OK)) {
                size_t n = 0; char *t = read_file(a, &n);
                FILE *f = fopen(b, "wb"); if (f) { if (t) fwrite(t, 1, n, f); else fputs("[TrackFiles]\r\n", f); fclose(f); ok = 1; }
                free(t);
            }
        }
        sb_printf(&out, "{\"ok\":%d}", ok); reply(fd, 200, "application/json", out.p, out.n);
    } else if (!strcmp(path, "/api/tolatin1") && !strcmp(method, "POST")) {
        /* a .set/.ini saved as UTF-8 -> Windows-1252, the encoding GPxPatch reads (also on Windows) */
        body[strcspn(body, "\r\n")] = 0; char p[PATH_MAX]; int ok = 0;
        if (file_path(body, p, sizeof p)) {
            size_t n = 0; char *t = read_file(p, &n);
            if (t && is_utf8_text(t, n)) {
                utf8_to_latin1(t);
                char tmp[PATH_MAX + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", p);
                FILE *f = fopen(tmp, "wb"); if (f) { fputs(t, f); fclose(f); ok = rename(tmp, p) == 0; }
            }
            free(t);
        }
        sb_printf(&out, "{\"ok\":%d}", ok); reply(fd, 200, "application/json", out.p, out.n);
    } else if (!strcmp(path, "/api/launch") && !strcmp(method, "POST")) {
        reap(); start_game(strstr(body, "gpx=1") != NULL);
        sb_printf(&out, "{\"running\":%d}", g_game > 0); reply(fd, 200, "application/json", out.p, out.n);
    } else if (!strcmp(path, "/api/quit") && !strcmp(method, "POST")) {
        reply(fd, 200, "application/json", "{}", 2); free(req.p); close(fd); hlog("launcher: closed by the page"); exit(0);
    } else reply(fd, 404, "text/plain", "not found", 9);
    free(out.p); free(req.p);
}

static void open_browser(const char *url) {
    pid_t p = fork();
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
        execlp("sensible-browser", "sensible-browser", url, (char *)NULL);
        execlp("firefox", "firefox", url, (char *)NULL);
        _exit(127);
    }
}
/* one launcher per user: a second start just opens the page of the running one */
static void state_file(char *out, size_t n) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) snprintf(out, n, "%s/gp3rt-gpxpatch.url", rt);
    else snprintf(out, n, "/tmp/gp3rt-gpxpatch-%d.url", (int)getuid());
}
static int already_running(void) {
    char sf[PATH_MAX]; state_file(sf, sizeof sf);
    char url[256] = ""; FILE *f = fopen(sf, "r"); if (!f) return 0;
    if (!fgets(url, sizeof url, f)) url[0] = 0; fclose(f);
    url[strcspn(url, "\r\n")] = 0;
    int port = 0; char dir[PATH_MAX] = "";
    if (sscanf(url, "http://127.0.0.1:%d/", &port) != 1) return 0;
    { FILE *g = fopen(sf, "r"); char l[PATH_MAX]; if (g) { if (fgets(l, sizeof l, g) && fgets(dir, sizeof dir, g)) dir[strcspn(dir, "\r\n")] = 0; fclose(g); } }
    if (strcmp(dir, g_dir)) return 0;   /* another game folder: start a new one */
    int s = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0; close(s);
    if (ok) open_browser(url);
    return ok;
}

/* ------------------------------------------------------------------ */
/* shared with the native screen (launcher.h)                           */
void lx_setup(const char *game_dir, const char *version) {
    VERSION = version;
    snprintf(g_dir, sizeof g_dir, "%s", game_dir);
    if (!find_ci(g_dir, "gpxpdata", g_gpxdir, sizeof g_gpxdir)) g_gpxdir[0] = 0;
}
const char *lx_dir(void) { return g_dir; }
const char *lx_gpxdir(void) { return g_gpxdir; }
const char *lx_version(void) { return VERSION; }
const char *lx_exe_version(void) { return exe_version(); }
int lx_find_ci(const char *dir, const char *name, char *out, size_t outsz) { return find_ci(dir, name, out, outsz); }
int lx_ci_walk(const char *base, const char *rel, char *out, size_t outsz) { return ci_walk(base, rel, out, outsz); }
int lx_game_file(const char *win, char *out, size_t outsz) { return game_file(win, out, outsz); }
char *lx_read_file(const char *path, size_t *len) { return read_file(path, len); }
int lx_file_path(const char *name, char *out, size_t outsz) { return file_path(name, out, outsz); }
int lx_ini_set(const char *path, const char *sec, const char *key, const char *val) { return ini_set(path, sec, key, val); }
char lx_set_kind(const char *path) { return set_kind(path); }
int lx_is_utf8(const char *s, size_t n) { return is_utf8_text(s, n); }
void lx_utf8_to_latin1(char *s) { utf8_to_latin1(s); }
int lx_make_shortcuts(void) { return make_shortcuts(); }
int lx_shortcuts_exist(void) {
    char p[PATH_MAX]; const char *h = getenv("HOME");
    snprintf(p, sizeof p, "%s/.local/share/applications/%s.desktop", h ? h : "/tmp", !strcmp(exe_version(), "2000") ? "gp3-2000-gpxpatch" : "gp3-gpxpatch");
    return access(p, F_OK) == 0;
}
void lx_start_game(int with_gpx) { start_game(with_gpx); }
int lx_game_running(void) { reap(); return g_game > 0; }
int lx_last_exit(void) { return g_last_exit; }

int launcher_main(const char *game_dir, const char *version) {
    lx_setup(game_dir, version);
    if (already_running()) return 0;
    { unsigned char r[16]; int fd = open("/dev/urandom", O_RDONLY); if (fd < 0 || read(fd, r, sizeof r) != sizeof r) { srand((unsigned)time(NULL) ^ (unsigned)getpid()); for (int i = 0; i < 16; i++) r[i] = (unsigned char)rand(); } if (fd >= 0) close(fd);
      for (int i = 0; i < 16; i++) sprintf(g_token + 2 * i, "%02x", r[i]); }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)) { fprintf(stderr, "launcher: cannot listen: %s\n", strerror(errno)); return 1; }
    socklen_t al = sizeof a; getsockname(s, (struct sockaddr *)&a, &al); g_port = ntohs(a.sin_port);
    char url[256]; snprintf(url, sizeof url, "http://127.0.0.1:%d/?t=%s", g_port, g_token);
    char sf[PATH_MAX]; state_file(sf, sizeof sf);
    { int fd = open(sf, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd >= 0) { FILE *f = fdopen(fd, "w"); fprintf(f, "%s\n%s\n", url, g_dir); fclose(f); } }
    hlog("launcher: %s (game folder %s, GPxPatch %s)", url, g_dir, g_gpxdir[0] ? "installed" : "not installed");
    printf("GPxPatch para Linux: abra %s\n", url); fflush(stdout);
    if (!getenv("GP3_LAUNCHER_NO_BROWSER")) open_browser(url);
    signal(SIGPIPE, SIG_IGN);
    g_last_seen = time(NULL);
    for (;;) {
        struct pollfd p = { s, POLLIN, 0 };
        int r = poll(&p, 1, 1000);
        reap();
        { int st; while (waitpid(-1, &st, WNOHANG) > 0) {} }   /* browser helper processes */
        if (r > 0) { int c = accept(s, NULL, NULL); if (c >= 0) { handle(c); close(c); } }
        /* the page polls every few seconds: when it has been closed and no game runs, stop */
        int idle = (int)(time(NULL) - g_last_seen);
        if (g_game <= 0 && g_bye && time(NULL) - g_bye >= 2) { hlog("launcher: page closed, exiting"); break; }
        if (g_game <= 0 && idle > 75) {   /* fallback (browser crashed): background tabs may poll only once a minute */ hlog("launcher: page gone, exiting"); break; }
    }
    unlink(sf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* GPxPatch 4.52 shipped with the package (original files, never changed) */
#define GPX_BUNDLE "GPxPatch-4.52"
int lx_bundled_gpx(void) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/" GPX_BUNDLE "/GPxPSup.dll", g_dir);
    return access(p, R_OK) == 0;
}
static int copy_tree_noclobber(const char *src, const char *dst) {
    DIR *d = opendir(src); if (!d) return -1;
    mkdir(dst, 0755);
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[PATH_MAX], t[PATH_MAX], have[PATH_MAX]; struct stat st;
        snprintf(s, sizeof s, "%s/%s", src, e->d_name);
        if (stat(s, &st)) continue;
        /* the game folder may already hold this name in another letter case */
        if (find_ci(dst, e->d_name, have, sizeof have)) snprintf(t, sizeof t, "%s", have);
        else snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
        if (S_ISDIR(st.st_mode)) { int k = copy_tree_noclobber(s, t); if (k < 0) { closedir(d); return -1; } n += k; continue; }
        if (access(t, F_OK) == 0) continue;                  /* never overwrite: the user's settings stay */
        FILE *in = fopen(s, "rb"), *out = in ? fopen(t, "wb") : NULL;
        if (!in || !out) { if (in) fclose(in); closedir(d); return -1; }
        char buf[65536]; size_t r;
        while ((r = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, r, out);
        fclose(in); if (fclose(out)) { closedir(d); return -1; }
        n++;
    }
    closedir(d); return n;
}
int lx_install_bundled_gpx(void) {
    if (!lx_bundled_gpx()) return -1;
    char s[PATH_MAX]; snprintf(s, sizeof s, "%s/" GPX_BUNDLE, g_dir);
    int n = copy_tree_noclobber(s, g_dir);
    if (!find_ci(g_dir, "gpxpdata", g_gpxdir, sizeof g_gpxdir)) g_gpxdir[0] = 0;
    hlog("GPxPatch 4.52 from the package installed into the game folder: %d file(s) copied", n);
    return n;
}
