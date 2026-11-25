/* GPxPatch for Linux - the GPxPatch configuration screen exactly as René Smit designed it.
 *
 * Nothing of the original is copied or changed: the windows are read, at run time, from the
 * language files GPxPatch itself installs (gpxpdata/lang/GPxResXX.dll: every dialog, its
 * controls, their positions and texts, in all of the author's translations) and drawn here in
 * the classic Windows look, scaled up (2x by default) for today's screens.
 * The settings go to the same .ini files the Windows GPxPatch uses (the gpxpdata .ini files).
 *
 * gpxclassic_main() returns -1 when it cannot run (no SDL 2.0.18, GPxPatch not installed and
 * not in the package); gp3.c then opens the other screen (host/gpxui.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <limits.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "nk_conf.h"
#include "launcher.h"
#include <iconv.h>
#include "classic_font.h"     /* generated: DejaVu Sans Condensed subset (Bitstream Vera licence) + Noto Sans SC subset (OFL) for the Chinese texts */

void hlog(const char *fmt, ...);
extern void set_window_icon_gpxp(SDL_Window *w);

/* ------------------------------------------------------------------ */
/* PE resources (the language DLLs)                                     */
typedef struct { uint8_t *d; size_t n; int ns; uint32_t va[24], vs[24], ro[24]; uint32_t rsrc; } pe_t;
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static int pe_off(const pe_t *p, uint32_t rva, uint32_t *off) {
    for (int i = 0; i < p->ns; i++) if (rva >= p->va[i] && rva < p->va[i] + p->vs[i]) { *off = rva - p->va[i] + p->ro[i]; return *off < p->n; }
    return 0;
}
static void pe_free(pe_t *p) { free(p->d); memset(p, 0, sizeof *p); }
static int pe_load(pe_t *p, const char *path) {
    memset(p, 0, sizeof *p);
    size_t n = 0; char *d = lx_read_file(path, &n);
    if (!d || n < 0x200) { free(d); return 0; }
    p->d = (uint8_t *)d; p->n = n;
    uint32_t pe = rd32(p->d + 0x3c); if (pe + 0xf8 > n || memcmp(p->d + pe, "PE\0\0", 4)) { pe_free(p); return 0; }
    p->ns = (int)rd16(p->d + pe + 6); if (p->ns > 24) p->ns = 24;
    uint32_t oh = rd16(p->d + pe + 20), opt = pe + 24, sec = opt + oh;
    uint32_t rsrc_rva = rd32(p->d + opt + 96 + 2 * 8);
    for (int i = 0; i < p->ns && sec + 40 * i + 40 <= n; i++) {
        const uint8_t *s = p->d + sec + 40 * i;
        p->vs[i] = rd32(s + 8); p->va[i] = rd32(s + 12); uint32_t rs = rd32(s + 16); p->ro[i] = rd32(s + 20);
        if (rs > p->vs[i]) p->vs[i] = rs;
    }
    if (!pe_off(p, rsrc_rva, &p->rsrc)) { pe_free(p); return 0; }
    return 1;
}
/* type/id -> data (first language) */
static const uint8_t *res_find(const pe_t *p, uint32_t type, uint32_t id, uint32_t *len) {
    uint32_t dir = p->rsrc; uint32_t want[2] = { type, id };
    for (int lvl = 0; lvl < 3; lvl++) {
        if (dir + 16 > p->n) return NULL;
        uint32_t nn = rd16(p->d + dir + 12), ni = rd16(p->d + dir + 14), next = 0; int found = 0;
        for (uint32_t i = 0; i < nn + ni; i++) {
            const uint8_t *e = p->d + dir + 16 + 8 * i; if (e + 8 > p->d + p->n) return NULL;
            uint32_t name = rd32(e), off = rd32(e + 4);
            if (lvl < 2 && ((name & 0x80000000) || name != want[lvl])) continue;
            found = 1; next = off; break;
        }
        if (!found) return NULL;
        if (next & 0x80000000) dir = p->rsrc + (next & 0x7fffffff);
        else {
            const uint8_t *de = p->d + p->rsrc + next; uint32_t o;
            if (de + 8 > p->d + p->n || !pe_off(p, rd32(de), &o)) return NULL;
            *len = rd32(de + 4); if (o + *len > p->n) return NULL;
            return p->d + o;
        }
    }
    return NULL;
}
/* UTF-16LE -> UTF-8 */
static size_t u16_to_u8(const uint8_t *s, size_t nchars, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < nchars; i++) {
        uint32_t c = rd16(s + 2 * i);
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < nchars) { uint32_t c2 = rd16(s + 2 * i + 2); c = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00); i++; }
        char b[4]; int k;
        if (c < 0x80) { b[0] = (char)c; k = 1; }
        else if (c < 0x800) { b[0] = (char)(0xc0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xe0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 63)); b[2] = (char)(0x80 | (c & 63)); k = 3; }
        else { b[0] = (char)(0xf0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 63)); b[2] = (char)(0x80 | (c >> 6 & 63)); b[3] = (char)(0x80 | (c & 63)); k = 4; }
        if (o + (size_t)k + 1 > outsz) break;
        memcpy(out + o, b, (size_t)k); o += (size_t)k;
    }
    if (outsz) out[o] = 0;
    return o;
}
/* ANSI text of a language file (the combo box items) -> UTF-8: Windows-1252, or the code page of
   that language (Russian 1251, Polish 1250, Chinese 936) */
static const char *g_codepage = "CP1252";
static void ansi_to_u8(const char *s, char *out, size_t outsz, int unused) {
    (void)unused;
    iconv_t cd = iconv_open("UTF-8", g_codepage);
    if (cd != (iconv_t)-1) {
        char *in = (char *)s, *o = out; size_t il = strlen(s), ol = outsz - 1;
        size_t r = iconv(cd, &in, &il, &o, &ol); iconv_close(cd);
        if (r != (size_t)-1) { *o = 0; return; }
    }
    size_t o = 0;   /* no iconv: Latin-1 */
    for (const unsigned char *c = (const unsigned char *)s; *c && o + 3 < outsz; c++) { if (*c < 0x80) out[o++] = (char)*c; else { out[o++] = (char)(0xc0 | *c >> 6); out[o++] = (char)(0x80 | (*c & 63)); } }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* dialog templates                                                     */
enum { K_BUTTON = 1, K_EDIT, K_STATIC, K_LIST, K_SCROLL, K_COMBO, K_TAB, K_LISTVIEW, K_OTHER };
typedef struct { int kind; char text[512]; int id; int x, y, w, h; uint32_t style, ex; } ctl_t;
typedef struct { int id; char caption[256]; int w, h; uint32_t style; int n; ctl_t *c; int en; } dlg_t;
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_TABSTOP 0x00010000u

static const uint8_t *sz_or_ord(const uint8_t *p, const uint8_t *end, uint32_t *ord, char *str, size_t strsz) {
    if (ord) *ord = 0; if (str && strsz) str[0] = 0;
    if (p + 2 > end) return end;
    uint32_t w = rd16(p);
    if (w == 0xffff) { if (ord) *ord = rd16(p + 2); return p + 4; }
    if (w == 0) return p + 2;
    const uint8_t *e = p; while (e + 2 <= end && rd16(e)) e += 2;
    if (str) u16_to_u8(p, (size_t)(e - p) / 2, str, strsz);
    return e + 2;
}
static const uint8_t *align4(const uint8_t *base, const uint8_t *p) { return base + ((size_t)(p - base + 3) & ~(size_t)3); }
static int dlg_parse(const uint8_t *d, uint32_t len, dlg_t *g) {
    memset(g, 0, sizeof *g);
    const uint8_t *p = d, *end = d + len; int ext = rd16(d) == 1 && rd16(d + 2) == 0xffff;
    int n;
    if (ext) { g->style = rd32(d + 12); n = (int)rd16(d + 16); g->w = (int16_t)rd16(d + 22); g->h = (int16_t)rd16(d + 24); p = d + 26; }
    else { g->style = rd32(d); n = (int)rd16(d + 8); g->w = (int16_t)rd16(d + 14); g->h = (int16_t)rd16(d + 16); p = d + 18; }
    p = sz_or_ord(p, end, NULL, NULL, 0);                         /* menu */
    p = sz_or_ord(p, end, NULL, NULL, 0);                         /* class */
    p = sz_or_ord(p, end, NULL, g->caption, sizeof g->caption);   /* caption */
    if (g->style & 0x40) { p += ext ? 6 : 2; p = sz_or_ord(p, end, NULL, NULL, 0); }   /* DS_SETFONT */
    g->c = calloc((size_t)(n > 0 ? n : 1), sizeof *g->c);
    for (int i = 0; i < n && p < end; i++) {
        ctl_t *c = &g->c[g->n];
        p = align4(d, p);
        if (ext) { c->ex = rd32(p + 4); c->style = rd32(p + 8); c->x = (int16_t)rd16(p + 12); c->y = (int16_t)rd16(p + 14); c->w = (int16_t)rd16(p + 16); c->h = (int16_t)rd16(p + 18); c->id = (int)rd32(p + 20); p += 24; }
        else { c->style = rd32(p); c->ex = rd32(p + 4); c->x = (int16_t)rd16(p + 8); c->y = (int16_t)rd16(p + 10); c->w = (int16_t)rd16(p + 12); c->h = (int16_t)rd16(p + 14); c->id = (int)rd16(p + 16); p += 18; }
        uint32_t ord; char cls[64];
        p = sz_or_ord(p, end, &ord, cls, sizeof cls);
        if (ord) c->kind = ord == 0x80 ? K_BUTTON : ord == 0x81 ? K_EDIT : ord == 0x82 ? K_STATIC : ord == 0x83 ? K_LIST : ord == 0x84 ? K_SCROLL : ord == 0x85 ? K_COMBO : K_OTHER;
        else if (!strcasecmp(cls, "SysTabControl32")) c->kind = K_TAB;
        else if (!strcasecmp(cls, "SysListView32")) c->kind = K_LISTVIEW;
        else if (!strcasecmp(cls, "Button")) c->kind = K_BUTTON;
        else if (!strcasecmp(cls, "Edit")) c->kind = K_EDIT;
        else if (!strcasecmp(cls, "Static")) c->kind = K_STATIC;
        else if (!strcasecmp(cls, "ComboBox")) c->kind = K_COMBO;
        else if (!strcasecmp(cls, "ListBox")) c->kind = K_LIST;
        else c->kind = K_OTHER;
        p = sz_or_ord(p, end, &ord, c->text, sizeof c->text);
        if (p + 2 > end) break;
        uint32_t extra = rd16(p); p += 2 + extra;
        if (c->id == 0xffff) c->id = -1;
        g->n++;
    }
    return 1;
}
static void dlg_free(dlg_t *g) { free(g->c); memset(g, 0, sizeof *g); }

/* ------------------------------------------------------------------ */
/* language files                                                       */
typedef struct { char code[8], name[64], path[PATH_MAX]; } lang_t;
static lang_t g_langs[16]; static int g_nlang, g_lang;
static pe_t g_res;
static int g_cp1251;
static char g_gpxdir[PATH_MAX];

static void version_comment(const pe_t *p, char *out, size_t outsz) {
    out[0] = 0; uint32_t len; const uint8_t *v = res_find(p, 16, 1, &len); if (!v) return;
    static const uint8_t key[] = { 'C',0,'o',0,'m',0,'m',0,'e',0,'n',0,'t',0,'s',0,0,0 };
    for (uint32_t i = 0; i + sizeof key < len; i += 2) if (!memcmp(v + i, key, sizeof key)) {
        const uint8_t *s = v + i + sizeof key; while ((size_t)(s - v) % 4) s++;
        const uint8_t *e = s; while (e + 2 <= v + len && rd16(e)) e += 2;
        u16_to_u8(s, (size_t)(e - s) / 2, out, outsz); return;
    }
}
static void scan_langs(void) {
    g_nlang = 0;
    char ld[PATH_MAX]; if (!lx_ci_walk(g_gpxdir, "lang", ld, sizeof ld)) return;
    DIR *d = opendir(ld); struct dirent *e;
    while (d && (e = readdir(d)) && g_nlang < 16) {
        size_t l = strlen(e->d_name);
        if (l != 12 || strncasecmp(e->d_name, "gpxres", 6) || strcasecmp(e->d_name + 8, ".dll")) continue;
        lang_t *L = &g_langs[g_nlang];
        snprintf(L->path, sizeof L->path, "%s/%s", ld, e->d_name);
        L->code[0] = (char)toupper((unsigned char)e->d_name[6]); L->code[1] = (char)toupper((unsigned char)e->d_name[7]); L->code[2] = 0;
        pe_t p; if (!pe_load(&p, L->path)) continue;
        version_comment(&p, L->name, sizeof L->name); if (!L->name[0]) snprintf(L->name, sizeof L->name, "%s", L->code);
        pe_free(&p); g_nlang++;
    }
    if (d) closedir(d);
    for (int i = 0; i < g_nlang; i++) for (int j = i + 1; j < g_nlang; j++)   /* English first, then by name */
        if ((strcmp(g_langs[j].code, "EN") == 0) || (strcmp(g_langs[i].code, "EN") && strcmp(g_langs[i].name, g_langs[j].name) > 0)) { lang_t t = g_langs[i]; g_langs[i] = g_langs[j]; g_langs[j] = t; }
}
static int load_lang(int i) {
    if (i < 0 || i >= g_nlang) return 0;
    pe_t p; if (!pe_load(&p, g_langs[i].path)) return 0;
    pe_free(&g_res); g_res = p; g_lang = i; g_cp1251 = !strcmp(g_langs[i].code, "RU");
    const char *c = g_langs[i].code; g_codepage = !strcmp(c, "RU") ? "CP1251" : !strcmp(c, "PL") ? "CP1250" : !strcmp(c, "CH") ? "CP936" : "CP1252";
    return 1;
}
/* some translations lack a few windows (the image control editor in PT, IT, RU): those come from the English file */
static pe_t g_res_en; static int g_res_en_tried;
static const pe_t *res_en(void) {
    if (!g_res_en_tried) { g_res_en_tried = 1; for (int k = 0; k < g_nlang; k++) if (!strcmp(g_langs[k].code, "EN")) pe_load(&g_res_en, g_langs[k].path); }
    return g_res_en.d ? &g_res_en : NULL;
}
static int get_dlg(int id, dlg_t *g) {
    uint32_t len; const uint8_t *d = res_find(&g_res, 5, (uint32_t)id, &len); int en = 0;
    if (!d && res_en()) { d = res_find(res_en(), 5, (uint32_t)id, &len); en = 1; }
    if (!d) return 0;
    const char *cp = g_codepage; if (en) g_codepage = "CP1252";
    dlg_parse(d, len, g); g->id = id; g->en = en; g_codepage = cp; return 1;
}
static const char *res_string(int id) {
    static char out[4][1024]; static int k; char *o = out[k++ & 3]; o[0] = 0;
    uint32_t len; const uint8_t *d = res_find(&g_res, 6, (uint32_t)(id / 16 + 1), &len); if (!d) return o;
    const uint8_t *p = d; for (int i = 0; i < 16 && p + 2 <= d + len; i++) { uint32_t n = rd16(p); p += 2; if (i == id % 16) { u16_to_u8(p, n, o, 1024); return o; } p += 2 * n; }
    return o;
}

/* ------------------------------------------------------------------ */
/* .ini values                                                          */
static char *ini_get(const char *file, const char *sec, const char *key, const char *def) {
    static char out[8][1024]; static int k; char *o = out[k++ & 7];
    snprintf(o, 1024, "%s", def);
    char path[PATH_MAX]; if (!lx_file_path(file, path, sizeof path)) return o;
    size_t n; char *t = lx_read_file(path, &n); if (!t) return o;
    int in = 0; char *line = t;
    while (line && *line) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        size_t l = strlen(s); while (l && (s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = 0;
        if (*s == '[') { char *e = strchr(s, ']'); if (e) { *e = 0; in = !strcasecmp(s + 1, sec); } }
        else if (in && *s && *s != ';') { char *eq = strchr(s, '='); if (eq) { *eq = 0; char *kk = s; size_t kl = strlen(kk); while (kl && kk[kl - 1] == ' ') kk[--kl] = 0;
            if (!strcasecmp(kk, key)) { char *v = eq + 1; while (*v == ' ') v++; char u[1024]; const char *cp = g_codepage; g_codepage = "CP1252"; ansi_to_u8(v, u, sizeof u, 0); g_codepage = cp; snprintf(o, 1024, "%s", lx_is_utf8(v, strlen(v)) ? v : u); free(t); return o; } } }
        line = nl ? nl + 1 : NULL;
    }
    free(t); return o;
}
static void ini_put(const char *file, const char *sec, const char *key, const char *val) {
    char path[PATH_MAX]; if (!lx_file_path(file, path, sizeof path)) return;
    char v[1024]; snprintf(v, sizeof v, "%s", val); lx_utf8_to_latin1(v);   /* GPxPatch files are Windows-1252 */
    lx_ini_set(path, sec, key, v);
}

/* ------------------------------------------------------------------ */
/* settings bound to controls (dialog, control) -> gpxpdata .ini key     */
typedef struct { int dlg, id; char kind; const char *file, *sec, *key, *def; } bind_t;
/* kinds: c checkbox 0/1, 3 three-state 0/1/2, i combo index, e edit text, t combo text */
static const bind_t BINDS[] = {
    /* GPxPatch */
    { 129, 1321, 'c', "gpxpatch.ini", "Mod", "gpxset", "1" },
    { 129, 1056, 'c', "gpxpatch.ini", "Mod", "gpxtrack", "1" },
    { 129, 1057, 'c', "gpxpatch.ini", "Mod", "gpxlap", "1" },
    { 129, 1062, 'c', "gpxpatch.ini", "Mod", "gpxsat", "1" },
    { 129, 1058, 'c', "gpxpatch.ini", "Mod", "gpxcinfo", "1" },
    { 129, 1089, 'c', "gpxpatch.ini", "Mod", "gpxsound", "1" },
    { 129, 1341, 'c', "gpxpatch.ini", "Mod", "gpxivibe", "0" },
    { 129, 1060, 'c', "gpxpatch.ini", "Game", "SkipIntro", "0" },
    { 129, 1053, 'c', "gpxpatch.ini", "Misc", "SkipStartupMenu", "0" },
    { 129, 1117, 'e', "gpxpatch.ini", "Misc", "HostIPAddress", "" },
    { 129, 1061, 't', "gpxpatch.ini", "Misc", "gp3exe", "gp3.exe" },
    { 129, 1002, 'c', "gpxpatch.ini", "Debug", "Debug", "0" },
    { 129, 1059, 'c', "gpxpatch.ini", "Debug", "LogAssertions", "0" },
    { 129, 1003, 'i', "gpxpatch.ini", "Debug", "Verbose", "0" },
    { 129, 1577, '3', "gpxpatch.ini", "Misc", "ExtendedPause", "0" },
    { 129, 1587, 'i', "gpxpatch.ini", "Debug", "CPU", "0" },
    /* GPxSet */
    { 145, 1196, 'c', "gpxset.ini", "SeasonManager", "Enable", "0" },
    { 145, 1197, 'e', "gpxset.ini", "SeasonManager", "SeasonSet", "season.set" },
    { 145, 1273, 'c', "gpxset.ini", "Misc", "TeamPower", "0" },
    { 145, 1635, 'i', "gpxset.ini", "Misc", "TeamPowerType", "3" },
    { 145, 1247, 'c', "gpxset.ini", "PerfManager", "Enable", "0" },
    { 145, 1248, 'e', "gpxset.ini", "PerfManager", "Path", "" },
    { 145, 1189, 'c', "gpxset.ini", "CarshapeManager", "Enable", "0" },
    { 145, 1190, 'e', "gpxset.ini", "CarshapeManager", "Carshape", "" },
    { 145, 1315, 'c', "gpxset.ini", "PhysicsManager", "Enable", "0" },
    { 145, 1316, 'e', "gpxset.ini", "PhysicsManager", "Path", "" },
    { 145, 1639, 'e', "gpxset.ini", "PhysicsManager", "TorquePath", "" },
    { 145, 1313, 't', "gpxset.ini", "Misc", "skya", "skya" },
    { 145, 1310, 'c', "gpxset.ini", "Misc", "DisableProgressBar", "0" },
    { 145, 1314, 't', "gpxset.ini", "Misc", "tarmac", "tarmacmap.bmp" },
    /* GPxTrack */
    { 132, 1013, 'c', "gpxtrack.ini", "TrackManager", "CatchMenuStrings", "1" },
    { 132, 1065, 'c', "gpxtrack.ini", "TrackManager", "CatchRecords", "0" },
    { 132, 1630, 'i', "gpxtrack.ini", "Misc", "ExtendTrackLength", "0" },
    { 132, 1637, 'i', "gpxtrack.ini", "Misc", "ExtendMaxRaceLaps", "0" },
    { 132, 1014, 't', "gpxtrack.ini", "TrackManager", "TrackSet", "tracks.set" },
    /* GPxLap */
    { 131, 1309, 'c', "gpxlap.ini", "Authentication", "Enable", "1" },
    { 131, 1347, 'c', "gpxlap.ini", "Authentication", "LogUniqueID", "0" },
    { 131, 1413, 'c', "gpxlap.ini", "Setup", "Enable", "0" },
    { 131, 1431, 'c', "gpxlap.ini", "SaveRestore", "Enable", "0" },
    { 131, 1640, 'c', "gpxlap.ini", "Quicksave", "Auto", "0" },
    { 131, 1004, 'c', "gpxlap.ini", "Misc", "BestSplit", "0" },
    { 131, 1607, 'c', "gpxlap.ini", "Misc", "HideReplayMenu", "0" },
    { 131, 1005, 'c', "gpxlap.ini", "Misc", "PitExtView", "0" },
    { 131, 1574, 'c', "gpxlap.ini", "Misc", "FixPitstopBug", "0" },
    { 131, 1336, 'c', "gpxlap.ini", "LapLog", "Enable", "0" },
    { 131, 1340, 'i', "gpxlap.ini", "LapLog", "Mode", "0" },
    { 131, 1338, 'e', "gpxlap.ini", "LapLog", "Path", "laps.log" },
    { 131, 1337, 'c', "gpxlap.ini", "LapLog", "Append", "0" },
    { 131, 1531, 't', "gpxlap.ini", "ATL", "Font", "ATL2004" },
    { 131, 1532, 't', "gpxlap.ini", "Log", "Font", "courier" },
    { 195, 1254, 'e', "gpxlap.ini", "EngineTemperature", "IdleLow", "24" },
    { 195, 1253, 'e', "gpxlap.ini", "EngineTemperature", "IdleHigh", "93" },
    { 195, 1252, 'e', "gpxlap.ini", "EngineTemperature", "Start", "93" },
    { 195, 1251, 'e', "gpxlap.ini", "EngineTemperature", "Maximum", "125" },
    { 195, 1255, 'e', "gpxlap.ini", "EngineTemperature", "ChangeSpeed", "16" },
    { 185, 1006, 'e', "gpxlap.ini", "RCR", "DistCockpit", "3840" },
    { 185, 1007, 'e', "gpxlap.ini", "RCR", "DistOnBoard", "3840" },
    { 185, 1008, 'e', "gpxlap.ini", "RCR", "DistChase", "7680" },
    { 185, 1009, 'e', "gpxlap.ini", "RCR", "DistTrack", "7680" },
    /* GPxSAT */
    { 138, 1063, 'c', "gpxsat.ini", "Misc", "ShowInCockpit", "0" },
    { 138, 1064, 'e', "gpxsat.ini", "Misc", "SpeedTrapWidth", "75" },
    { 138, 1342, 'i', "gpxsat.ini", "Misc", "Mode", "3" },
    { 138, 1110, 'c', "gpxsat.ini", "Misc", "ReplaceSkyMessagesWhenShowInCockpitIsOff", "0" },
    { 138, 1067, 'c', "gpxsat.ini", "Sky", "ReplaceFastestLap", "1" },
    { 138, 1069, 'c', "gpxsat.ini", "Sky", "ReplaceOutOfRace", "1" },
    { 138, 1068, 'c', "gpxsat.ini", "Sky", "ReplaceIsInPits", "1" },
    { 138, 1070, 'c', "gpxsat.ini", "Sky", "ReplaceHasWonRace", "1" },
    { 138, 1101, 'c', "gpxsat.ini", "Sky", "ReplaceRidingWith", "1" },
    { 138, 1566, 'c', "gpxsat.ini", "Sky", "ReplaceBlackFlag", "1" },
    { 138, 1185, 'e', "gpxsat.ini", "MicroTime", "PosX", "10" },
    { 138, 1186, 'e', "gpxsat.ini", "MicroTime", "PosY", "10" },
    { 138, 1586, 't', "gpxsat.ini", "Font", "Font2D", "courier" },
    /* GPxCInfo */
    { 139, 1344, 'c', "gpxcinfo.ini", "Export", "Enable", "0" },
    /* GPxSound */
    { 140, 1114, 'c', "gpxsound.ini", "Engine", "Enabled", "1" },
    { 140, 1119, 'c', "gpxsound.ini", "Engine", "DisableGPxSound3D", "1" },
    { 140, 1094, 'e', "gpxsound.ini", "Engine", "Wave", "engine.wav" },
    { 140, 1096, 'e', "gpxsound.ini", "Engine", "MinDistance", "4." },
    { 140, 1097, 'e', "gpxsound.ini", "Engine", "MaxDistance", "200." },
    { 140, 1099, 'e', "gpxsound.ini", "Engine", "Revs2PitchFactor", "1.2" },
    { 140, 1100, 'e', "gpxsound.ini", "Engine", "Revs2PitchOffset", "4000" },
    { 140, 1098, 'e', "gpxsound.ini", "Engine", "Revs2VolumeFactor", "0.2" },
    { 140, 1102, 'e', "gpxsound.ini", "Engine", "Revs2VolumeOffset", "-2000" },
    { 140, 1610, 'c', "gpxsound.ini", "Siren", "Enabled", "1" },
    { 140, 1101, 'e', "gpxsound.ini", "Siren", "Wave", "pithorn.wav" },
    { 140, 1104, 'e', "gpxsound.ini", "Siren", "MinDistance", "5." },
    { 140, 1105, 'e', "gpxsound.ini", "Siren", "MaxDistance", "500." },
    { 140, 1113, 'c', "gpxsound.ini", "EventSound", "Enabled", "1" },
    { 140, 1614, 'e', "gpxsound.ini", "EventSound", "Volume", "0" },
    { 140, 1111, 'e', "gpxsound.ini", "EventSound", "SoundSet", "soundset.ini" },
    { 205, 1091, 'e', "gpxsound.ini", "Listener", "DopplerFactor", "1." },
    { 205, 1092, 'e', "gpxsound.ini", "Listener", "RolloffFactor", "1." },
    { 205, 1093, 'e', "gpxsound.ini", "Listener", "DistanceFactor", "1." },
    { 205, 1115, 'c', "gpxsound.ini", "Engine", "UseVoiceMan", "1" },
    { 205, 1108, 'i', "gpxsound.ini", "Engine", "3DAlgorithm", "0" },
    /* GPxSet session settings */
    { 169, 1322, 'e', "gpxset.ini", "Misc", "QuickraceLength", "5" },
    { 169, 1590, 'i', "gpxset.ini", "Misc", "Weather", "0" },
    { 169, 1591, 'e', "gpxset.ini", "Misc", "RainChance", "50" },
    { 169, 1312, 'i', "gpxset.ini", "Random", "Type", "0" },
    { 169, 1311, 'e', "gpxset.ini", "Random", "Seed", "" },
    { 169, 1589, 'i', "gpxset.ini", "Random", "Session", "0" },
    { 169, 1428, 'e', "gpxset.ini", "Misc", "FreePracticeCars", "1" },
    { 169, 1429, 'c', "gpxset.ini", "FriPrac", "UseQualMenu", "0" },
    { 169, 1416, 'e', "gpxset.ini", "Misc", "ConsecutiveCars", "13" },
    { 169, 1417, 'e', "gpxset.ini", "FreePrac", "ConsecutiveCars", "" },
    { 169, 1418, 'e', "gpxset.ini", "FriPrac", "ConsecutiveCars", "" },
    { 169, 1419, 'e', "gpxset.ini", "SatPrac", "ConsecutiveCars", "" },
    { 169, 1420, 'e', "gpxset.ini", "SatQual", "ConsecutiveCars", "" },
    { 169, 1421, 'e', "gpxset.ini", "Warmup", "ConsecutiveCars", "" },
    { 169, 1422, 'e', "gpxset.ini", "Misc", "FuelLaps", "4" },
    { 169, 1423, 'e', "gpxset.ini", "FreePrac", "FuelLaps", "" },
    { 169, 1424, 'e', "gpxset.ini", "FriPrac", "FuelLaps", "" },
    { 169, 1425, 'e', "gpxset.ini", "SatPrac", "FuelLaps", "" },
    { 169, 1426, 'e', "gpxset.ini", "SatQual", "FuelLaps", "" },
    { 169, 1427, 'e', "gpxset.ini", "Warmup", "FuelLaps", "" },
    { 169, 1631, 'e', "gpxset.ini", "FriPrac", "Laps", "23" },
    { 169, 1632, 'e', "gpxset.ini", "SatPrac", "Laps", "23" },
    { 169, 1633, 'e', "gpxset.ini", "SatQual", "Laps", "12" },
    /* season set: car sets & files */
    { 158, 1184, 'c', "$season", "CarSets", "Enable", "0" },
    { 158, 1250, 'c', "$season", "CarSets", "EnableTrackSpecific", "0" },
    { 158, 1180, 'e', "$season", "CarSets", "CarSet", "" },          /* key changes with the track chosen in 1204 */
    { 158, 1187, 'c', "$season", "CarSets", "DisableCarNrCopying", "0" },
    { 158, 1323, 'c', "$season", "TrackSet", "Enable", "0" },
    { 158, 1324, 'e', "$season", "TrackSet", "Path", "" },
    { 158, 1195, 'c', "$season", "Carshape", "Enable", "0" },
    { 158, 1193, 'e', "$season", "Carshape", "Carshape", "" },
    { 158, 1318, 'c', "$season", "Physics", "Enable", "0" },
    { 158, 1319, 'e', "$season", "Physics", "Path", "" },
    { 158, 1321, 'e', "$season", "Physics", "TorquePath", "" },
    { 158, 1410, 'c', "$season", "MenuGfx", "Enable", "0" },
    { 158, 1411, 'e', "$season", "MenuGfx", "Path", "bitmaps\\gp2000v1.bin" },
    { 158, 1608, 'c', "$season", "SATMode", "Enable", "0" },
    { 158, 1342, 'i', "$season", "SATMode", "Mode", "0" },
    /* season set: tyres */
    { 156, 1263, 'c', "$season", "Tyres", "Enable", "1" },
    { 156, 1237, 'e', "$season", "Tyres", "Jam01_01", "" },
    { 156, 1239, 'e', "$season", "Tyres", "Jam01_02", "" },
    { 156, 1238, 'e', "$season", "Tyres", "Jam02_01", "" },
    { 156, 1240, 'e', "$season", "Tyres", "Jam02_02", "" },
    { 156, 1264, 'e', "$season", "Tyres", "TreadArt01_01", "" },
    { 156, 1266, 'e', "$season", "Tyres", "TreadArt01_02", "" },
    { 156, 1268, 'e', "$season", "Tyres", "TreadArt02_01", "" },
    { 156, 1270, 'e', "$season", "Tyres", "TreadArt02_02", "" },
    { 156, 1272, 'e', "$season", "Tyres", "Tread", "tread" },
    { 156, 1276, 'e', "$season", "Tyres", "SatType01", "" },
    { 156, 1279, 'e', "$season", "Tyres", "SatType02", "" },
    /* car set: teams/drivers and track */
    { 154, 1247, 'c', "$set", "Performance", "Enable", "0" },
    { 154, 1248, 'e', "$set", "Performance", "Path", "" },
    { 154, 1284, 'c', "$set", "TeamPitOrder", "Enable", "0" },
    { 155, 1259, 'c', "$set", "Track", "Enable", "0" },
    { 155, 1260, 'e', "$set", "Track", "Path", "" },
    { 155, 1274, 'e', "$set", "TrackJams", "Podgen04", "gp3jams\\genjam\\podgen04.jam" },
    { 155, 1346, 't', "$set", "TrackJams", "Tarmac", "" },
    /* season set: points */
    { 160, 1308, 'c', "$season", "RacePoints", "Enable", "1" },
    { 0 }
};
/* "browse" buttons: (dialog, button) -> the edit box they fill, where the file is looked for, its type */
typedef struct { int dlg, btn, edit; char root; const char *ext; } browse_t;   /* root: g game folder, p gpxpdata, s gpxpdata/snd, m gp3jams/main, t gp3jamsh/techjam */
static const browse_t BROWSE[] = {
    { 145, 1198, 1197, 'p', "set" }, { 145, 1249, 1248, 'g', "txt" }, { 145, 1191, 1190, 'g', "cs3" }, { 145, 1317, 1316, 'g', "g3p" }, { 145, 1318, 1639, 'g', "ini" },
    { 132, 1129, 1014, 'p', "set" }, { 131, 1339, 1338, 'g', "log" },
    { 140, 1095, 1094, 's', "wav" }, { 140, 1103, 1101, 's', "wav" }, { 140, 1112, 1111, 's', "ini" },
    { 158, 1182, 1180, 'p', "set" }, { 158, 1325, 1324, 'p', "set" }, { 158, 1194, 1193, 'g', "cs3" }, { 158, 1320, 1319, 'g', "g3p" }, { 158, 1322, 1321, 'g', "ini" }, { 158, 1412, 1411, 'g', "bin" },
    { 156, 1243, 1237, 'm', "jam" }, { 156, 1245, 1239, 'm', "jam" }, { 156, 1244, 1238, 'm', "jam" }, { 156, 1246, 1240, 'm', "jam" },
    { 156, 1265, 1264, 't', "bmp" }, { 156, 1267, 1266, 't', "bmp" }, { 156, 1269, 1268, 't', "bmp" }, { 156, 1271, 1270, 't', "bmp" },
    { 156, 1278, 1276, 'p', "tga" }, { 156, 1280, 1279, 'p', "tga" },
    { 167, 1477, 1402, 'p', "ttf" }, { 138, 1107, 1103, 'x', "tga" }, { 138, 1096, 1104, 'x', "tga" },
    { 154, 1249, 1248, 'g', "txt" }, { 155, 1261, 1260, 'g', "dat" }, { 155, 1275, 1274, 'g', "jam" }, { 155, 1277, 1346, 't', "bmp" },
    { 0 }
};
/* "$season" and "$set": the .set file a dialog is editing */
static char g_setfile[256];
static const char *season_file(void) { const char *v = ini_get("gpxset.ini", "SeasonManager", "SeasonSet", "season.set"); return *v ? v : "season.set"; }
static const char *bind_file(const bind_t *b) {
    if (!strcmp(b->file, "$season")) return season_file();
    if (!strcmp(b->file, "$set")) return g_setfile;
    return b->file;
}
static const bind_t *find_bind(int dlg, int id) { for (const bind_t *b = BINDS; b->dlg; b++) if (b->dlg == dlg && b->id == id) return b; return NULL; }

/* ------------------------------------------------------------------ */
/* the button pictures of GPxPatch.exe (open, new, up, down, delete), read from the author's file */
static SDL_Texture *g_bmp[8]; static int g_bmp_id[8], g_nbmp;
static struct SDL_Renderer *g_ren;
static void load_bitmaps(void) {
    char ex[PATH_MAX]; pe_t p; if (!lx_find_ci(lx_dir(), "GPxPatch.exe", ex, sizeof ex) || !pe_load(&p, ex)) return;
    static const int ids[] = { 500, 502, 505, 506, 507 };
    for (int k = 0; k < 5 && g_nbmp < 8; k++) {
        uint32_t len; const uint8_t *d = res_find(&p, 2, (uint32_t)ids[k], &len); if (!d || len < 40) continue;
        uint32_t hs = rd32(d); int w = (int)rd32(d + 4), h = (int)rd32(d + 8), bpp = (int)rd16(d + 14); uint32_t clr = rd32(d + 32);
        if (bpp != 4 || w <= 0 || w > 64 || h <= 0 || h > 64) continue;
        uint32_t nc = clr ? clr : 16; const uint8_t *pal = d + hs, *bits = pal + nc * 4; int stride = ((w * 4 + 31) / 32) * 4;
        if (bits + stride * h > d + len) continue;
        uint32_t px[64 * 64];
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            int b = bits[(h - 1 - y) * stride + x / 2]; int c = x & 1 ? b & 15 : b >> 4;
            uint8_t B = pal[c * 4], G = pal[c * 4 + 1], R = pal[c * 4 + 2];
            int bg = R == 192 && G == 192 && B == 192;   /* the button face shows through */
            px[y * w + x] = bg ? 0 : (0xffu << 24 | (uint32_t)R << 16 | (uint32_t)G << 8 | B);
        }
        SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!t) continue;
        SDL_UpdateTexture(t, NULL, px, w * 4); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
        g_bmp[g_nbmp] = t; g_bmp_id[g_nbmp++] = ids[k];
    }
    pe_free(&p);
}
static SDL_Texture *bitmap(int id) { for (int k = 0; k < g_nbmp; k++) if (g_bmp_id[k] == id) return g_bmp[k]; return NULL; }
/* which picture each picture button shows (as GPxPatch sets them up) */
static int button_bitmap(int dlg, int id) {
    if (dlg == 139) switch (id) { case 1366: return 500; case 1521: return 502; case 1624: return 507; case 1622: return 505; case 1623: return 506; }
    if (dlg == 161) switch (id) { case 1567: return 502; case 1568: return 507; case 1569: return 505; case 1570: return 506; }   /* panel items: new, delete, up, down */
    if (dlg == 181) switch (id) { case 1480: return 502; case 1481: return 507; case 1518: return 505; case 1519: return 506; }
    return 500;   /* the "browse" buttons: open folder */
}

/* the original GPxPatch icon (64x64, 32-bit) from the author's GPxPatch.exe: window icon, and the
   menu / desktop / file manager icon of "GPxPatch" (written into the user's icon theme) */
extern int write_png_rgba(const char *path, const uint32_t *argb, int w, int h);
static uint32_t g_icon[64 * 64]; static int g_have_icon;
static SDL_Window *g_win;
static void original_icon(void) {
    char ex[PATH_MAX]; pe_t p;
    if (!lx_find_ci(lx_dir(), "GPxPatch.exe", ex, sizeof ex) || !pe_load(&p, ex)) return;
    uint32_t len; const uint8_t *g = res_find(&p, 14, 128, &len);
    if (g && len >= 20) {
        int n = (int)rd16(g + 4), best = -1, bw = 0;
        for (int k = 0; k < n && 6 + 14 * k + 14 <= (int)len; k++) { const uint8_t *e = g + 6 + 14 * k; int w = e[0] ? e[0] : 256; if (rd16(e + 6) == 32 && w <= 64 && w > bw) { bw = w; best = (int)rd16(e + 12); } }
        uint32_t il; const uint8_t *d = best >= 0 ? res_find(&p, 3, (uint32_t)best, &il) : NULL;
        if (d && il >= 40 + 64 * 64 * 4 && rd32(d) == 40 && rd32(d + 4) == 64 && rd16(d + 14) == 32) {
            const uint8_t *px = d + 40;
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { const uint8_t *q = px + ((63 - y) * 64 + x) * 4; g_icon[y * 64 + x] = (uint32_t)q[3] << 24 | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0]; }
            g_have_icon = 1;
        }
    }
    pe_free(&p);
    if (!g_have_icon) return;
    SDL_Surface *sf = SDL_CreateRGBSurfaceFrom(g_icon, 64, 64, 32, 64 * 4, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
    if (sf) { SDL_SetWindowIcon(g_win, sf); SDL_FreeSurface(sf); }
    const char *h = getenv("HOME"); if (!h) return;
    static const int sizes[] = { 16, 24, 32, 48, 64, 128, 256 };
    static uint32_t big[256 * 256];
    for (int k = 0; k < 7; k++) {
        int n = sizes[k]; char d[PATH_MAX], f[PATH_MAX + 64];
        snprintf(d, sizeof d, "%s/.local/share/icons/hicolor/%dx%d/apps", h, n, n);
        { char t[PATH_MAX]; snprintf(t, sizeof t, "%s", d); for (char *c = t + strlen(h) + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(t, 0755); *c = '/'; } mkdir(t, 0755); }
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) big[y * n + x] = g_icon[(y * 64 / n) * 64 + x * 64 / n];   /* nearest: keeps the pixel look */
        snprintf(f, sizeof f, "%s/gpxpatch-linux.png", d);
        write_png_rgba(f, big, n, n);
    }
}

/* ------------------------------------------------------------------ */
/* drawing                                                              */
static struct nk_font *g_font, *g_cjk; static SDL_Texture *g_ftex; static int g_ftw, g_fth;
static float S = 2.0f;               /* scale of the classic look (1 = the original size) */
static int T = 1;                    /* the thickness of a 1-pixel line at that scale */
static float g_hidpi = 1;
#define BX 6
#define BY 13
static int DX(int d) { return (int)((float)d * BX * S / 4.0f + 0.5f); }
static int DY(int d) { return (int)((float)d * BY * S / 8.0f + 0.5f); }
typedef struct { Uint8 r, g, b; } col_t;
static const col_t C_FACE = { 212, 208, 200 }, C_HI = { 255, 255, 255 }, C_SH = { 128, 128, 128 }, C_DK = { 64, 64, 64 },
                   C_WIN = { 255, 255, 255 }, C_TXT = { 0, 0, 0 }, C_GRAY = { 128, 128, 128 }, C_SEL = { 10, 36, 106 }, C_SELT = { 255, 255, 255 };
static void fill(int x, int y, int w, int h, col_t c) { if (w <= 0 || h <= 0) return; SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, 255); SDL_Rect r = { x, y, w, h }; SDL_RenderFillRect(g_ren, &r); }
static void hline(int x, int y, int w, col_t c) { fill(x, y, w, T, c); }
static void vline(int x, int y, int h, col_t c) { fill(x, y, T, h, c); }
/* a 1-pixel classic frame, S pixels thick: top/left in a, bottom/right in b */
static void frame(int x, int y, int w, int h, col_t a, col_t b) { hline(x, y, w - T, a); vline(x, y, h - T, a); hline(x, y + h - T, w, b); vline(x + w - T, y, h, b); }
static void raised(int x, int y, int w, int h) { frame(x, y, w, h, C_HI, C_DK); frame(x + T, y + T, w - 2 * T, h - 2 * T, C_FACE, C_SH); }
static void sunken(int x, int y, int w, int h) { frame(x, y, w, h, C_SH, C_HI); frame(x + T, y + T, w - 2 * T, h - 2 * T, C_DK, C_FACE); }
static void pressed_btn(int x, int y, int w, int h) { frame(x, y, w, h, C_DK, C_DK); frame(x + T, y + T, w - 2 * T, h - 2 * T, C_SH, C_SH); }
/* pixel art at the classic size, each pixel S x S */
static void art(int x, int y, const char *const *rows, int nrows, col_t c) {
    for (int r = 0; r < nrows; r++) for (int i = 0; rows[r][i]; i++) if (rows[r][i] == '#') {
        int x0 = (int)(i * S + 0.5f), x1 = (int)((i + 1) * S + 0.5f), y0 = (int)(r * S + 0.5f), y1 = (int)((r + 1) * S + 0.5f);
        fill(x + x0, y + y0, x1 - x0, y1 - y0, c);
    }
}
static const char *const CHECK[] = { "      #", "     ##", "#   ###", "## ### ", "#####  ", " ###   ", "  #    " };
static const char *const ARROW[] = { "#######", " ##### ", "  ###  ", "   #   " };
static const char *const RADIO_OUT_A[] = { "    ####", "  ##    ", " #      ", " #      ", "#       ", "#       ", "#       ", "#       ", " #      ", " #      " };

/* text */
static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s; int c = *p;
    if (c < 0x80) { *s += 1; return c; }
    if ((c & 0xe0) == 0xc0 && p[1]) { *s += 2; return (c & 0x1f) << 6 | (p[1] & 0x3f); }
    if ((c & 0xf0) == 0xe0 && p[1] && p[2]) { *s += 3; return (c & 0x0f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f); }
    if ((c & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) { *s += 4; return (c & 0x07) << 18 | (p[1] & 0x3f) << 12 | (p[2] & 0x3f) << 6 | (p[3] & 0x3f); }
    *s += 1; return '?';
}
#define CX 0.9f   /* DejaVu Sans Condensed squeezed a little more: the width of MS Sans Serif, for which the dialogs were laid out */
static const struct nk_font_glyph *glyph(int cp, float *cx) {
    const struct nk_font_glyph *g = nk_font_find_glyph(g_font, (nk_rune)cp); *cx = CX;
    if ((!g || g->codepoint != (nk_rune)cp) && g_cjk && cp >= 0x2e80) { const struct nk_font_glyph *h = nk_font_find_glyph(g_cjk, (nk_rune)cp); if (h && h->codepoint == (nk_rune)cp) { g = h; *cx = 1; } }
    return g;
}
static float glyph_adv(int cp) { float cx; const struct nk_font_glyph *g = glyph(cp, &cx); return g ? g->xadvance * cx : 0; }
/* width of a text with '&' mnemonics (n bytes, or -1 for all) */
static int text_w(const char *s, int n) {
    float w = 0; const char *e = n < 0 ? s + strlen(s) : s + n;
    while (s < e && *s) { if (*s == '&') { s++; if (*s != '&') continue; } w += glyph_adv(utf8_next(&s)); }
    return (int)(w + 0.5f);
}
static int line_h(void) { return DY(8); }
static void draw_run(float x, int y, const char *s, int n, col_t c, int *ux, int *uw) {
    SDL_SetTextureColorMod(g_ftex, c.r, c.g, c.b);
    const char *e = n < 0 ? s + strlen(s) : s + n; int under = 0;
    while (s < e && *s) {
        if (*s == '&') { s++; if (*s != '&') { under = 1; continue; } }
        int cp = utf8_next(&s); float CXg;
        const struct nk_font_glyph *g = glyph(cp, &CXg);
        if (!g) continue;
        if (g->w > 0 && cp != ' ') {
            SDL_Rect src = { (int)(g->u0 * g_ftw), (int)(g->v0 * g_fth), (int)((g->u1 - g->u0) * g_ftw + 0.5f), (int)((g->v1 - g->v0) * g_fth + 0.5f) };
            SDL_FRect dst = { x + g->x0 * CXg, (float)y + g->y0, (g->x1 - g->x0) * CXg, (g->y1 - g->y0) };
            SDL_RenderCopyF(g_ren, g_ftex, &src, &dst);
        }
        if (under && ux) { *ux = (int)x; *uw = (int)(g->xadvance * CXg + 0.5f); under = 0; }
        x += g->xadvance * CXg;
    }
}
/* one line of text at x,y (top); mnemonic underline */
static void text(int x, int y, const char *s, int n, col_t c) {
    int ux = -1, uw = 0; draw_run((float)x, y, s, n, c, &ux, &uw);
    if (ux >= 0) fill(ux, y + (int)(g_font->info.ascent + 0.5f) + S, uw, S > 1 ? S / 2 + 0 : 1, c);
}
/* disabled text: classic embossed look */
static void text_state(int x, int y, const char *s, int n, int enabled) {
    if (enabled) text(x, y, s, n, C_TXT); else { text(x + S / 2 + 1, y + S / 2 + 1, s, n, C_HI); text(x, y, s, n, C_GRAY); }
}
/* word-wrapped text in a box (SS_LEFT / SS_CENTER / SS_RIGHT, multi-line buttons) */
static int text_box(int x, int y, int w, int h, const char *s, int align, int enabled, int draw) {
    int lh = line_h(), yy = y, lines = 0;
    const char *p = s;
    while (*p) {
        if (*p == '\n') { p++; yy += lh; lines++; continue; }
        const char *best = NULL, *q = p;
        while (*q && *q != '\n') {
            const char *r = q; while (*r && *r != ' ' && *r != '\n') r++;
            if (text_w(p, (int)(r - p)) > w && best) break;
            best = r; q = r; while (*q == ' ') q++;
            if (text_w(p, (int)(best - p)) > w) break;
        }
        if (!best) best = q;
        int len = (int)(best - p), tw = text_w(p, len);
        int tx = align == 1 ? x + (w - tw) / 2 : align == 2 ? x + w - tw : x;
        if (draw && yy + lh <= y + h + lh / 2) text_state(tx, yy, p, len, enabled);
        p = best; while (*p == ' ') p++;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
        yy += lh; lines++;
    }
    return lines * lh;
}

/* ------------------------------------------------------------------ */
/* the window model                                                     */
#define MAXC 160
typedef struct { int ival; char sval[512]; int nitems; char **items; int sel_scroll; int iscolor, multi; } val_t;
typedef struct {
    dlg_t g; int ox, oy;              /* origin in window pixels */
    val_t v[MAXC]; int hidden[MAXC], disabled[MAXC];
} page_t;
static page_t g_main, g_page;
/* dialogs opened over the main window: a stack (the season editor opens the track set editor...) */
typedef struct {
    page_t pg, sub;                       /* the dialog, and the page of its tab control (if any) */
    int ntabs, tab, tabdlg[8], tabparam[8]; const char *tabname[8];
    int save_w, save_h, save_x, save_y;   /* the window size and place before it grew for this dialog */
    char bkfile[64]; char *bktext; size_t bklen; int immediate;   /* writes at once; Cancel puts the file back */
    char file[256];                       /* the .set file the dialog edits */
    int hidden_set;
    int scroll, scroll_max;               /* a dialog taller than the screen scrolls with the mouse wheel */
} modal_t;
static modal_t g_ms[4]; static int g_nm;
#define g_in_modal (g_nm > 0)
#define MT (g_ms[g_nm - 1])
static page_t *PG(int i) { if (i >= 2) { modal_t *m = &g_ms[(i - 2) / 2]; return (i & 1) ? &m->sub : &m->pg; } return i ? &g_page : &g_main; }
static int pg_of(page_t *p) { if (p == &g_main) return 0; if (p == &g_page) return 1; for (int k = 0; k < g_nm; k++) { if (p == &g_ms[k].pg) return 2 + 2 * k; if (p == &g_ms[k].sub) return 3 + 2 * k; } return 0; }
static int top_pg(void) { return 2 + 2 * (g_nm - 1); }
static int g_tab;                   /* current main tab */
typedef struct { int dlg; const char *name; } tabdef_t;
static const tabdef_t TABS[] = { { 129, "GPxPatch" }, { 145, "GPxSet" }, { 132, "GPxTrack" }, { 131, "GPxLap" }, { 138, "GPxSAT" }, { 139, "GPxCInfo" }, { 140, "GPxSound" } };
#define NTABS ((int)(sizeof TABS / sizeof TABS[0]))
static int g_focus_pg = -1, g_focus = -1, g_caret;    /* edit with keyboard focus: page (0 main, 1 tab page), control */
static int g_drop_pg = -1, g_drop = -1, g_drop_hover = -1, g_drop_scroll;   /* open combo list */
static int g_press_pg = -1, g_press = -1, g_hover_in;
static int g_quit, g_is2000;

static int ci_has(const char *h, const char *n) { size_t l = strlen(n); for (; *h; h++) if (!strncasecmp(h, n, l)) return 1; return 0; }
static void free_items(val_t *v) { for (int i = 0; i < v->nitems; i++) free(v->items[i]); free(v->items); v->items = NULL; v->nitems = 0; }
static void add_item(val_t *v, const char *s) { v->items = realloc(v->items, sizeof(char *) * (size_t)(v->nitems + 1)); v->items[v->nitems++] = strdup(s); }
static void page_clear(page_t *p) { for (int i = 0; i < p->g.n && i < MAXC; i++) free_items(&p->v[i]); dlg_free(&p->g); memset(p, 0, sizeof *p); }
static int idx_of(page_t *p, int id) { for (int i = 0; i < p->g.n && i < MAXC; i++) if (p->g.c[i].id == id) return i; return -1; }

/* combo contents from the dialog's DLGINIT, plus the lists GPxPatch fills itself */
static void dlginit(page_t *p) {
    uint32_t len; const uint8_t *d = res_find(p->g.en && res_en() ? res_en() : &g_res, 240, (uint32_t)p->g.id, &len);
    for (const uint8_t *q = d; d && q + 8 <= d + len; ) {
        uint32_t id = rd16(q); if (!id) break;
        uint32_t n = rd32(q + 4); if (q + 8 + n > d + len) break;
        char a[512]; size_t m = n < sizeof a - 1 ? n : sizeof a - 1; memcpy(a, q + 8, m); a[m] = 0;
        char u[1024]; ansi_to_u8(a, u, sizeof u, g_cp1251);
        int i = idx_of(p, (int)id); if (i >= 0) add_item(&p->v[i], u);
        q += 8 + n;
    }
}
static void pitorder_list(page_t *p);
static void carset_labels(page_t *p);
static void pnl_list(page_t *p); static void pnl_fonts(page_t *p); static void ini_drop_section(const char *file, const char *sec);
static void changed(page_t *p, int pg, int i); static int pick_file(const char *start, const char *ext, char *out, size_t outsz);
static void fill_lists(page_t *p) {
    int i;
    if (p->g.id == 102 && (i = idx_of(p, 1327)) >= 0) { for (int k = 0; k < g_nlang; k++) add_item(&p->v[i], g_langs[k].name); p->v[i].ival = g_lang; }
    if (p->g.id == 129 && (i = idx_of(p, 1061)) >= 0) {   /* GPx executable: the .exe files of the game folder */
        DIR *d = opendir(lx_dir()); struct dirent *e;
        while (d && (e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 4 && !strcasecmp(e->d_name + l - 4, ".exe") && strncasecmp(e->d_name, "gpxpatch", 8) && strncasecmp(e->d_name, "uninst", 6) && strncasecmp(e->d_name, "register", 8)) add_item(&p->v[i], e->d_name); }
        if (d) closedir(d);
    }
    if (p->g.id == 129 && (i = idx_of(p, 1587)) >= 0) { int n = SDL_GetCPUCount(); for (int k = 1; k <= n && k < 64; k++) { char b[8]; snprintf(b, sizeof b, "%d", k); add_item(&p->v[i], b); } }
    char dir[PATH_MAX];
    if (p->g.id == 145 && lx_ci_walk(lx_dir(), "gp3jamsh/techjam", dir, sizeof dir)) {   /* sky prefixes and tarmac bitmaps */
        int is = idx_of(p, 1313), it = idx_of(p, 1314); DIR *d = opendir(dir); struct dirent *e;
        while (d && (e = readdir(d))) { const char *n = e->d_name; size_t l = strlen(n);
            if (l < 8 || strcasecmp(n + l - 4, ".bmp")) continue;
            if (!strncasecmp(n, "sky", 3) && l >= 9 && isdigit((unsigned char)n[l - 5]) && isdigit((unsigned char)n[l - 6])) {
                char pre[64]; size_t pl = l - 6 < sizeof pre - 1 ? l - 6 : sizeof pre - 1; memcpy(pre, n, pl); pre[pl] = 0;
                for (char *c = pre; *c; c++) *c = (char)tolower((unsigned char)*c);
                int have = 0; if (is >= 0) { for (int k = 0; k < p->v[is].nitems; k++) if (!strcmp(p->v[is].items[k], pre)) have = 1; if (!have) add_item(&p->v[is], pre); }
            } else if (it >= 0 && ci_has(n, "tarmac")) add_item(&p->v[it], n);
        }
        if (d) closedir(d);
    }
    if (p->g.id == 132 && (i = idx_of(p, 1014)) >= 0) {   /* track sets in gpxpdata */
        DIR *d = opendir(g_gpxdir); struct dirent *e;
        while (d && (e = readdir(d))) { size_t l = strlen(e->d_name); if (l < 5 || strcasecmp(e->d_name + l - 4, ".set")) continue;
            char f[PATH_MAX]; snprintf(f, sizeof f, "%s/%s", g_gpxdir, e->d_name); if (lx_set_kind(f) == 't') add_item(&p->v[i], e->d_name); }
        if (d) closedir(d);
        { int have = 0; for (int k = 0; k < p->v[i].nitems; k++) if (!strcasecmp(p->v[i].items[k], "tracks.set")) have = 1;   /* the default set, even before it exists */
          if (!have) add_item(&p->v[i], "tracks.set"); }
    }
    if ((p->g.id == 131 || p->g.id == 138) ) {   /* fonts defined in fonts.ini */
        static const int ids[] = { 1531, 1532, 1586 };
        char fp[PATH_MAX]; size_t n; char *t = lx_file_path("fonts.ini", fp, sizeof fp) ? lx_read_file(fp, &n) : NULL;
        for (int k = 0; k < 3; k++) { if ((i = idx_of(p, ids[k])) < 0) continue;
            for (char *q = t; q && *q; ) { char *nl = strchr(q, '\n'); if (*q == '[') { char *e = strchr(q, ']'); if (e && (!nl || e < nl)) { char sec[128]; size_t l = (size_t)(e - q - 1) < sizeof sec - 1 ? (size_t)(e - q - 1) : sizeof sec - 1; memcpy(sec, q + 1, l); sec[l] = 0; add_item(&p->v[i], sec); } } q = nl ? nl + 1 : NULL; } }
        free(t);
    }
    if (p->g.id == 155 && (i = idx_of(p, 1346)) >= 0 && lx_ci_walk(lx_dir(), "gp3jamsh/techjam", dir, sizeof dir)) {
        DIR *d = opendir(dir); struct dirent *e; while (d && (e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 4 && !strcasecmp(e->d_name + l - 4, ".bmp") && ci_has(e->d_name, "tarmac")) add_item(&p->v[i], e->d_name); } if (d) closedir(d);
    }
    if (p->g.id == 161) { pnl_list(p); pnl_fonts(p); }
    if (p->g.id == 196) { int a = idx_of(p, 1599), b2 = idx_of(p, 1593);
        if (a >= 0) for (int k = 0; k < 6; k++) add_item(&p->v[a], res_string(5220 + k));
        if (b2 >= 0) for (int k = 0; k < 6; k++) add_item(&p->v[b2], res_string(5200 + k)); }
    if (p->g.id == 154) pitorder_list(p);
    if (p->g.id == 153) carset_labels(p);
    if (p->g.id == 158 && (i = idx_of(p, 1204)) >= 0 && !p->v[i].nitems) { add_item(&p->v[i], "--"); for (int k = 1; k <= 17; k++) { char b[8]; snprintf(b, sizeof b, "%02d", k); add_item(&p->v[i], b); } }
    if (p->g.id == 139 && (i = idx_of(p, 1621)) >= 0) {   /* GPxCInfo panels: [Panel] EnableN / PathN */
        for (int k = 1; k < 100; k++) { char kp[16], ke[16]; snprintf(kp, sizeof kp, "Path%d", k); snprintf(ke, sizeof ke, "Enable%d", k);
            const char *pth = ini_get("gpxcinfo.ini", "Panel", kp, ""); if (!*pth) break;
            char it[600]; snprintf(it, sizeof it, "%c%s", atoi(ini_get("gpxcinfo.ini", "Panel", ke, "1")) ? '1' : '0', pth); add_item(&p->v[i], it); }
        p->v[i].ival = -1;
    }
}
/* GPxTrack: the 17 tracks of the chosen set, under the list */
static void track_labels(page_t *p) {
    if (p->g.id != 132) return;
    char set[300]; snprintf(set, sizeof set, "%s", ini_get("gpxtrack.ini", "TrackManager", "TrackSet", "tracks.set")); if (!set[0]) snprintf(set, sizeof set, "tracks.set");
    for (int k = 0; k < 17; k++) {
        int i = idx_of(p, 1015 + k); if (i < 0) continue;
        char key[16], def[64]; snprintf(key, sizeof key, "f1ct%02d", k + 1); snprintf(def, sizeof def, "Circuits\\F1CT%02d.dat", k + 1);
        const char *v = ini_get(set, "TrackFiles", key, def);   /* a missing file or line: the game's own track */
        const char *b = strrchr(v, '\\'); if (!b) b = strrchr(v, '/'); b = b ? b + 1 : v;
        snprintf(p->g.c[i].text, sizeof p->g.c[i].text, "%02d: %s", k + 1, b);
    }
}
/* car set pages: jam lists (as GPxPatch numbers them: team, a/b, jam id) */
static const struct { const char *sec; int count, base, team_step, car_step; } CARSET_JAMS[] = {
    { "Cars", 22, 289, 3, 0 }, { "Cockpits", 22, 1108, 1, 0 }, { "Helmets", 3, 545, 0, 1 } };
static int g_cs_cat;      /* the jam list on show (page 153) */
static int g_multi;       /* the track set window used for "Multi..." (performance file per track) */
static void carset_labels(page_t *p) {
    if (p->g.id != 153) return;
    int c = g_cs_cat;
    for (int i = 0; i < 22; i++) {
        int on = i < CARSET_JAMS[c].count, k;
        int id = CARSET_JAMS[c].base + (i / 2) * CARSET_JAMS[c].team_step + i * CARSET_JAMS[c].car_step;
        if ((k = idx_of(p, 1206 + i)) >= 0) { snprintf(p->g.c[k].text, sizeof p->g.c[k].text, "%d%c (%d):", i / 2 + 1, 'a' + (i & 1), id); p->hidden[k] = !on; }
        if ((k = idx_of(p, 1131 + i)) >= 0) p->hidden[k] = !on;
        if ((k = idx_of(p, 1155 + i)) >= 0) p->hidden[k] = !on;
    }
}
/* the pit order list of the car set: Pos01..Pos11 = team */
static void pitorder_list(page_t *p) {
    int i = idx_of(p, 1281); if (i < 0) return;
    int keep = p->v[i].ival; free_items(&p->v[i]);
    for (int k = 0; k < 11; k++) { char key[16], def[8], it[32]; snprintf(key, sizeof key, "Pos%02d", k + 1); snprintf(def, sizeof def, "%d", k + 1);
        snprintf(it, sizeof it, "Team %02d", atoi(ini_get(g_setfile, "TeamPitOrder", key, def))); add_item(&p->v[i], it); }
    p->v[i].ival = keep >= 0 && keep < 11 ? keep : 0;
}
static void pitorder_move(page_t *p, int d) {
    int i = idx_of(p, 1281); if (i < 0) return;
    int a = p->v[i].ival, b = a + d; if (a < 0 || b < 0 || b >= 11) return;
    char ka[16], kb[16], da[8], db[8], va[16], vb[16]; snprintf(ka, sizeof ka, "Pos%02d", a + 1); snprintf(kb, sizeof kb, "Pos%02d", b + 1); snprintf(da, sizeof da, "%d", a + 1); snprintf(db, sizeof db, "%d", b + 1);
    snprintf(va, sizeof va, "%s", ini_get(g_setfile, "TeamPitOrder", ka, da)); snprintf(vb, sizeof vb, "%s", ini_get(g_setfile, "TeamPitOrder", kb, db));
    ini_put(g_setfile, "TeamPitOrder", ka, vb); ini_put(g_setfile, "TeamPitOrder", kb, va);
    p->v[i].ival = b; pitorder_list(p);
}

/* ------------------------------------------------------------------ */
/* GPxCInfo panel editor (dialog 161) and the windows it opens          */
static char g_pnl[300];           /* the panel being edited: pnl\<folder>\<panel>.ini */
static char g_pnl_sec[128];       /* the section of the chosen info item */
static int g_pnl_sel = -1;
static int ini_sections(const char *file, char out[][128], int max);
typedef struct { int dlg, id; char kind, where; const char *sec, *key, *def; } pbind_t;   /* where: f fixed section, i item, c item control, m control image */
static const pbind_t PBINDS[] = {
    { 161, 1575, 'c', 'f', "Filter", "Enable", "0" }, { 161, 1441, 'c', 'f', "Range", "Enable", "0" }, { 161, 1487, 'c', 'f', "Images", "Enable", "0" },
    { 161, 1520, 'c', 'f', "Misc", "DrawTransparentPanels", "0" }, { 161, 1578, 'e', 'f', "Misc", "Left", "0" }, { 161, 1580, 'e', 'f', "Misc", "Top", "0" },
    { 161, 1579, 'e', 'f', "Misc", "Right", "1024" }, { 161, 1581, 'e', 'f', "Misc", "Bottom", "768" }, { 161, 1628, 'i', 'f', "Misc", "Scaling", "0" },
    { 161, 1391, 't', 'f', "Misc", "Font", "" }, { 161, 1445, 'k', 'f', "Misc", "TextColor", "0x00ffffff" }, { 161, 1446, 'e', 'f', "Misc", "TextAlpha", "1." },
    { 161, 1387, 'c', 'i', 0, "Enable", "1" }, { 161, 1605, 'e', 'i', 0, "Name", "" }, { 161, 1433, 'c', 'i', 0, "EnableText", "1" },
    { 161, 1385, 'e', 'i', 0, "X", "0" }, { 161, 1386, 'e', 'i', 0, "Y", "0" }, { 161, 1626, 'i', 'i', 0, "Align", "0" }, { 161, 1615, 'i', 'i', 0, "Decimals", "0" },
    { 161, 1438, 'e', 'i', 0, "Prefix", "" }, { 161, 1439, 'e', 'i', 0, "Postfix", "" }, { 161, 1388, 't', 'i', 0, "Font", "" },
    { 161, 1448, 'i', 'i', 0, "ColorFixed", "0" }, { 161, 1444, 'k', 'i', 0, "TextColor", "0x00ffffff" }, { 161, 1447, 'e', 'i', 0, "TextAlpha", "1." },
    { 161, 1449, 'k', 'i', 0, "TextColor2", "0x00ffffff" }, { 161, 1450, 'e', 'i', 0, "TextAlpha2", "1." }, { 161, 1459, 'i', 'i', 0, "ControlType", "0" },
    { 161, 1435, 'e', 'i', 0, "Min", "0." }, { 161, 1437, 'e', 'i', 0, "Max", "0." }, { 161, 1565, 'e', 'i', 0, "TopFreezeDuration", "0" },
    /* the page of each item type */
    { 174, 1538, 'c', 'i', 0, "UseImages", "0" }, { 174, 1539, 'c', 'i', 0, "UsePosForImages", "0" }, { 174, 1536, 'k', 'i', 0, "TextColorOff", "0x00808080" }, { 174, 1537, 'k', 'i', 0, "TextColorBusy", "0x00ff0000" },
    { 175, 1449, 'c', 'i', 0, "ShowLapsLeft", "0" }, { 175, 1450, 'c', 'i', 0, "ShowTotalLaps", "0" }, { 175, 1638, 'c', 'i', 0, "RaceMode", "0" },
    { 176, 1451, 'e', 'i', 0, "NameLength", "0" }, { 176, 1634, 'e', 'i', 0, "Sign", "" },
    { 177, 1453, 'c', 'i', 0, "ShowMillis", "0" }, { 177, 1455, 'e', 'i', 0, "FreezeDuration", "0" },
    { 178, 1456, 'c', 'i', 0, "ShowDiffWhenFrozen", "0" }, { 178, 1457, 'e', 'i', 0, "FreezeDuration", "0" }, { 178, 1458, 'k', 'i', 0, "TextColorFaster", "0x0000ff00" }, { 178, 1459, 'k', 'i', 0, "TextColorSlower", "0x00ff0000" },
    { 189, 1538, 'c', 'i', 0, "UseImages", "0" }, { 189, 1539, 'c', 'i', 0, "UsePosForImages", "0" }, { 189, 1541, 'k', 'i', 0, "TextColorOff", "0x00808080" }, { 189, 1542, 'k', 'i', 0, "TextColorDisabled", "0x00404040" }, { 189, 1543, 'k', 'i', 0, "TextColorDisabledOn", "0x00ffffff" },
    { 204, 1603, 'e', 'i', 0, "AvgFrames", "15" },
    { 206, 1617, 'c', 'i', 0, "CountDown", "0" }, { 206, 1618, 'e', 'i', 0, "Minutes", "0" },
    { 207, 1538, 'c', 'i', 0, "UseImages", "0" }, { 207, 1539, 'c', 'i', 0, "UsePosForImages", "0" }, { 207, 1536, 'k', 'i', 0, "TextColorOff", "0x00808080" },
    { 208, 1538, 'c', 'i', 0, "UseImages", "0" }, { 208, 1539, 'c', 'i', 0, "UsePosForImages", "0" },
    { 209, 1538, 'c', 'i', 0, "UseImages", "0" }, { 209, 1539, 'c', 'i', 0, "UsePosForImages", "0" }, { 209, 1620, 'c', 'i', 0, "DisableSkyMessages", "0" },
    /* Filter */
    { 196, 1430, 'i', 'f', "Filter", "ShowInReplay", "1" }, { 196, 1392, 'i', 'f', "Filter", "ShowInPits", "1" }, { 196, 1598, 'i', 'f', "Filter", "ShowForPlayer", "1" },
    { 196, 1576, 'i', 'f', "Filter", "ShowInSAT", "1" }, { 196, 1599, 'm', 'f', "Filter", "SessionMode", "0,1,2,3,4,5" }, { 196, 1593, 'm', 'f', "Filter", "Camera", "0,1,2,3,4,5" },
    /* rotating line control */
    { 179, 1461, 'c', 'c', 0, "Enable", "1" }, { 179, 1468, 'e', 'c', 0, "AngleStart", "180" }, { 179, 1469, 'e', 'c', 0, "AngleEnd", "270" }, { 179, 1470, 'c', 'c', 0, "Clockwise", "1" },
    { 179, 1462, 'c', 'c', 0, "CutOff", "1" }, { 179, 1463, 'e', 'c', 0, "X", "0" }, { 179, 1464, 'e', 'c', 0, "Y", "0" }, { 179, 1465, 'e', 'c', 0, "Offset", "0" },
    { 179, 1466, 'e', 'c', 0, "Length", "60" }, { 179, 1467, 'c', 'c', 0, "Scale", "1" }, { 179, 1471, 'k', 'c', 0, "Color", "0x00ff0000" },
    /* image control */
    { 190, 1551, 'c', 'c', 0, "Enable", "1" }, { 190, 1550, 'i', 'c', 0, "CutOff", "1" }, { 190, 1553, 'i', 'c', 0, "Mode", "2" },
    { 190, 1482, 'e', 'm', 0, "Path", "" }, { 190, 1484, 'e', 'm', 0, "X", "0" }, { 190, 1485, 'e', 'm', 0, "Y", "0" }, { 190, 1582, 'e', 'm', 0, "Width", "0" }, { 190, 1583, 'e', 'm', 0, "Height", "0" },
    { 190, 1488, 'e', 'm', 0, "Alpha", "1." }, { 190, 1486, 'c', 'm', 0, "Scale", "1" },
    { 190, 1489, 'e', 'm', 0, "SrcX", "0" }, { 190, 1490, 'e', 'm', 0, "SrcY", "0" }, { 190, 1491, 'e', 'm', 0, "SrcWidth", "0" }, { 190, 1492, 'e', 'm', 0, "SrcHeight", "0" },
    { 193, 1554, 'e', 'c', 0, "AngleStart", "180" }, { 193, 1555, 'e', 'c', 0, "AngleEnd", "270" }, { 193, 1556, 'c', 'c', 0, "Clockwise", "1" },
    { 194, 1546, 'e', 'c', 0, "WeightLeft", "0." }, { 194, 1545, 'e', 'c', 0, "WeightTop", "0." }, { 194, 1547, 'e', 'c', 0, "WeightRight", "0." }, { 194, 1548, 'e', 'c', 0, "WeightBottom", "0." },
    { 194, 1602, 'e', 'c', 0, "XOffset", "0" }, { 194, 1603, 'e', 'c', 0, "YOffset", "0" },
    { 199, 1594, 'e', 'c', 0, "XOffset", "0" }, { 199, 1595, 'e', 'c', 0, "YOffset", "0" },
    { 203, 1600, 'e', 'c', 0, "Threshold", "0." }, { 203, 1619, 'e', 'c', 0, "ThresholdHi", "0." },
    { 0, 0, 0, 0, 0, 0, 0 } };
static const pbind_t *find_pbind(int dlg, int id) { for (const pbind_t *b = PBINDS; b->dlg; b++) if (b->dlg == dlg && b->id == id) return b; return NULL; }
/* the page under the item fields, by item type (as GPxPatch builds it) */
static int pnl_type_page(int t) {
    switch (t) { case 5: return 175; case 7: case 8: return 176; case 9: return 174; case 10: return 177; case 11: return 178; case 26: return 204; case 29: return 206;
    case 32: case 33: case 34: return 207; case 35: return 209; }
    return t >= 13 && t <= 21 ? 189 : 0;
}
static int pnl_count(void) { return atoi(ini_get(g_pnl, "Items", "Count", "0")); }
static void pnl_item(int k, char *out, size_t n) { char key[16]; snprintf(key, sizeof key, "Item%02d", k + 1); snprintf(out, n, "%s", ini_get(g_pnl, "Items", key, "")); }
static void pnl_dir(char *out, size_t n) {   /* pnl\<folder> */
    snprintf(out, n, "%s", g_pnl); char *b = strrchr(out, '\\'); if (!b) b = strrchr(out, '/'); if (b) *b = 0; else out[0] = 0;
}
static void pnl_list(page_t *p) {
    int i = idx_of(p, 1384); if (i < 0) return;
    free_items(&p->v[i]); int n = pnl_count();
    for (int k = 0; k < n; k++) { char sec[128]; pnl_item(k, sec, sizeof sec);
        const char *nm = ini_get(g_pnl, sec, "Name", ""); int t = atoi(ini_get(g_pnl, sec, "Type", "0"));
        char lab[300]; snprintf(lab, sizeof lab, "%s", *nm ? nm : (t >= 0 && t < 38 ? res_string(5080 + t) : sec)); if (!lab[0]) snprintf(lab, sizeof lab, "%s", sec);
        add_item(&p->v[i], lab); }
    if (g_pnl_sel >= n) g_pnl_sel = n - 1;
    if (g_pnl_sel < 0 && n > 0) g_pnl_sel = 0;
    if (g_pnl_sel >= 0) pnl_item(g_pnl_sel, g_pnl_sec, sizeof g_pnl_sec); else g_pnl_sec[0] = 0;
    p->v[i].ival = g_pnl_sel;
    { int vis = (DY(p->g.c[i].h) - 4 * (int)S) / (line_h() + 2 * (int)S); if (vis < 1) vis = 1;   /* keep the chosen item in view */
      if (g_pnl_sel >= 0 && g_pnl_sel < p->v[i].sel_scroll) p->v[i].sel_scroll = g_pnl_sel; if (g_pnl_sel >= p->v[i].sel_scroll + vis) p->v[i].sel_scroll = g_pnl_sel - vis + 1; }
}
static void pnl_fonts(page_t *p) {   /* the fonts of the panel's folder, then the general ones */
    static const int ids[] = { 1391, 1388 }; char loc[320], d[300]; pnl_dir(d, sizeof d); snprintf(loc, sizeof loc, "%s\\fonts.ini", d);
    static char a[64][128], b[64][128]; int na = ini_sections(loc, a, 64), nb = ini_sections("fonts.ini", b, 64);
    for (int k = 0; k < 2; k++) { int i = idx_of(p, ids[k]); if (i < 0) continue; free_items(&p->v[i]);
        for (int q = 0; q < na; q++) add_item(&p->v[i], a[q]);
        for (int q = 0; q < nb; q++) { int dup = 0; for (int r = 0; r < na; r++) if (!strcasecmp(a[r], b[q])) dup = 1; if (!dup) add_item(&p->v[i], b[q]); } }
}
static void ini_del_key(const char *file, const char *sec, const char *key) {
    char fp[PATH_MAX]; size_t n; char *t = lx_file_path(file, fp, sizeof fp) ? lx_read_file(fp, &n) : NULL; if (!t) return;
    FILE *f = fopen(fp, "wb"); if (!f) { free(t); return; }
    int in = 0; size_t kl = strlen(key);
    for (char *q = t; q && *q; ) { char *nl = strchr(q, '\n'); size_t l = nl ? (size_t)(nl - q + 1) : strlen(q);
        char *b = q; while (*b == ' ') b++;
        if (*b == '[') { char *e = strchr(b, ']'); in = e && (size_t)(e - b - 1) == strlen(sec) && !strncasecmp(b + 1, sec, strlen(sec)); }
        int drop = in && !strncasecmp(b, key, kl) && (b[kl] == '=' || b[kl] == ' ');
        if (!drop) fwrite(q, 1, l, f);
        q = nl ? nl + 1 : NULL; }
    fclose(f); free(t);
}
static int sec_exists(const char *file, const char *sec) { static char s[256][128]; int n = ini_sections(file, s, 256); for (int k = 0; k < n; k++) if (!strcasecmp(s[k], sec)) return 1; return 0; }
static void pnl_add(int type) {
    static const char *const base[38] = { "Gear", "Revs", "Speed", "Fuel", "EngineTemp", "Lap", "Position", "GapFront", "GapBehind", "Pit", "LapTime", "BestLapTime", "SessionTime",
        "HelpF1", "HelpF2", "HelpF3", "HelpF4", "HelpF5", "HelpF6", "HelpF7", "HelpF8", "HelpF9", "Steer", "Throttle", "Brake", "Name", "PO", "Label", "SuggestedGear", "GameTime",
        "LongAcc", "LatAcc", "BlueFlag", "YellowFlag", "Warning", "HotseatState", "GearChanges", "PitTimer" };
    if (type < 0 || type >= 38) return;
    char sec[64]; for (int n = 0;; n++) { if (n) snprintf(sec, sizeof sec, "%s%d", base[type], n); else snprintf(sec, sizeof sec, "%s", base[type]); if (!sec_exists(g_pnl, sec)) break; }
    int c = pnl_count(); char v[16], key[16]; snprintf(v, sizeof v, "%d", c + 1); ini_put(g_pnl, "Items", "Count", v); snprintf(key, sizeof key, "Item%02d", c + 1); ini_put(g_pnl, "Items", key, sec);
    snprintf(v, sizeof v, "%d", type); ini_put(g_pnl, sec, "Type", v); ini_put(g_pnl, sec, "Enable", "1"); ini_put(g_pnl, sec, "Name", "");
    ini_put(g_pnl, sec, "EnableText", "1"); ini_put(g_pnl, sec, "X", "0"); ini_put(g_pnl, sec, "Y", "0"); ini_put(g_pnl, sec, "Align", "0");
    ini_put(g_pnl, sec, "Decimals", "0"); ini_put(g_pnl, sec, "Prefix", ""); ini_put(g_pnl, sec, "Postfix", ""); ini_put(g_pnl, sec, "ColorFixed", "0"); ini_put(g_pnl, sec, "ControlType", "0");
    g_pnl_sel = c;
}
static void pnl_delete(void) {
    int c = pnl_count(), i = g_pnl_sel; if (i < 0 || i >= c) return;
    char sec[128]; pnl_item(i, sec, sizeof sec);
    for (int j = i; j < c - 1; j++) { char a[128], key[16]; pnl_item(j + 1, a, sizeof a); snprintf(key, sizeof key, "Item%02d", j + 1); ini_put(g_pnl, "Items", key, a); }
    char key[16], v[16]; snprintf(key, sizeof key, "Item%02d", c); ini_del_key(g_pnl, "Items", key); snprintf(v, sizeof v, "%d", c - 1); ini_put(g_pnl, "Items", "Count", v);
    int used = 0; for (int j = 0; j < c - 1; j++) { char a[128]; pnl_item(j, a, sizeof a); if (!strcasecmp(a, sec)) used = 1; }
    if (!used) { char cs[200]; ini_drop_section(g_pnl, sec); snprintf(cs, sizeof cs, "%sControl", sec); ini_drop_section(g_pnl, cs);
        for (int n = 0; n < 8; n++) { snprintf(cs, sizeof cs, "%sControlImage%d", sec, n); ini_drop_section(g_pnl, cs); } }
    if (g_pnl_sel >= c - 1) g_pnl_sel = c - 2;
}
static void pnl_move(int d) {
    int c = pnl_count(), i = g_pnl_sel, j = i + d; if (i < 0 || j < 0 || j >= c) return;
    char a[128], b[128], ka[16], kb[16]; pnl_item(i, a, sizeof a); pnl_item(j, b, sizeof b); snprintf(ka, sizeof ka, "Item%02d", i + 1); snprintf(kb, sizeof kb, "Item%02d", j + 1);
    ini_put(g_pnl, "Items", ka, b); ini_put(g_pnl, "Items", kb, a); g_pnl_sel = j;
}
/* the panels of GPxCInfo ([Panel] PathN / EnableN in gpxcinfo.ini) */
static int cinfo_count(void) { int k = 0; for (;; k++) { char kp[16]; snprintf(kp, sizeof kp, "Path%d", k + 1); if (!*ini_get("gpxcinfo.ini", "Panel", kp, "")) return k; } }
static void cinfo_swap(int a, int b) {
    char ka[16], kb[16], ea[16], eb[16], pa[300], pb[300], va[8], vb[8];
    snprintf(ka, sizeof ka, "Path%d", a + 1); snprintf(kb, sizeof kb, "Path%d", b + 1); snprintf(ea, sizeof ea, "Enable%d", a + 1); snprintf(eb, sizeof eb, "Enable%d", b + 1);
    snprintf(pa, sizeof pa, "%s", ini_get("gpxcinfo.ini", "Panel", ka, "")); snprintf(pb, sizeof pb, "%s", ini_get("gpxcinfo.ini", "Panel", kb, ""));
    snprintf(va, sizeof va, "%s", ini_get("gpxcinfo.ini", "Panel", ea, "1")); snprintf(vb, sizeof vb, "%s", ini_get("gpxcinfo.ini", "Panel", eb, "1"));
    ini_put("gpxcinfo.ini", "Panel", ka, pb); ini_put("gpxcinfo.ini", "Panel", kb, pa); ini_put("gpxcinfo.ini", "Panel", ea, vb); ini_put("gpxcinfo.ini", "Panel", eb, va);
}
static void cinfo_remove(int k) {
    int n = cinfo_count(); if (k < 0 || k >= n) return;
    for (int j = k; j < n - 1; j++) cinfo_swap(j, j + 1);
    char kp[16], ke[16]; snprintf(kp, sizeof kp, "Path%d", n); snprintf(ke, sizeof ke, "Enable%d", n); ini_del_key("gpxcinfo.ini", "Panel", kp); ini_del_key("gpxcinfo.ini", "Panel", ke);
}
/* colours: 0x00RRGGBB (or a decimal number), picked with the desktop's colour chooser */
static int color_pick(unsigned long *val) {
    unsigned long n = *val;
    char cmd[256]; snprintf(cmd, sizeof cmd, "zenity --color-selection --title='GPxPatch' --color='rgb(%lu,%lu,%lu)' 2>/dev/null", (n >> 16) & 255, (n >> 8) & 255, n & 255);
    FILE *f = popen(cmd, "r"); char b[64] = ""; if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; pclose(f); }
    unsigned r, g, bl;
    if (sscanf(b, "rgb(%u,%u,%u)", &r, &g, &bl) == 3 || sscanf(b, "rgba(%u,%u,%u", &r, &g, &bl) == 3) { *val = (n & 0xff000000ul) | (unsigned long)(r << 16 | g << 8 | bl); return 1; }
    if (b[0] == '#' && strlen(b) >= 7) { *val = (n & 0xff000000ul) | (strtoul(b + 1, NULL, 16) & 0xffffff); return 1; }
    return 0;
}
/* the .ini value behind a control: the fixed table, or keys that depend on another control */
typedef struct { char kind; char file[256], sec[64], key[64], def[64]; } rb_t;
static int sel_of(page_t *p, int id) { int i = idx_of(p, id); return i >= 0 ? p->v[i].ival : 0; }
static int resolve(page_t *p, int i, rb_t *r) {
    int id = p->g.c[i].id, dlg = p->g.id;
    const pbind_t *pb = g_pnl[0] ? find_pbind(dlg, id) : NULL;
    if (pb) {
        if (pb->where != 'f' && !g_pnl_sec[0]) return 0;
        r->kind = pb->kind; snprintf(r->file, sizeof r->file, "%s", g_pnl); snprintf(r->key, sizeof r->key, "%s", pb->key); snprintf(r->def, sizeof r->def, "%s", pb->def);
        if (pb->where == 'f') snprintf(r->sec, sizeof r->sec, "%s", pb->sec);
        else if (pb->where == 'i') snprintf(r->sec, sizeof r->sec, "%s", g_pnl_sec);
        else if (pb->where == 'c') snprintf(r->sec, sizeof r->sec, "%.50sControl", g_pnl_sec);
        else snprintf(r->sec, sizeof r->sec, "%.50sControlImage0", g_pnl_sec);
        return 1;
    }
    const bind_t *b = find_bind(dlg, id);
    if (b) {
        r->kind = b->kind; snprintf(r->file, sizeof r->file, "%s", bind_file(b)); snprintf(r->sec, sizeof r->sec, "%s", b->sec); snprintf(r->key, sizeof r->key, "%s", b->key); snprintf(r->def, sizeof r->def, "%s", b->def);
        if (dlg == 158 && id == 1180) { int k = sel_of(p, 1204); if (k > 0) snprintf(r->key, sizeof r->key, "CarSet%02d", k); }
        return 1;
    }
    if (dlg == 160 && id >= 1286 && id <= 1307) {   /* race points of each position */
        static const int def[22] = { 10, 6, 4, 3, 2, 1 }; int k = id - 1286;
        r->kind = 'e'; snprintf(r->file, sizeof r->file, "%s", season_file()); snprintf(r->sec, sizeof r->sec, "RacePoints"); snprintf(r->key, sizeof r->key, "Pos%02d", k + 1); snprintf(r->def, sizeof r->def, "%d", def[k]);
        return 1;
    }
    if (dlg == 156 && (id == 1241 || id == 1242)) {   /* tyre type of the team chosen in 1236 */
        r->kind = 'r'; snprintf(r->file, sizeof r->file, "%s", season_file()); snprintf(r->sec, sizeof r->sec, "Tyres"); snprintf(r->key, sizeof r->key, "Team%02d", sel_of(p, 1236) + 1); snprintf(r->def, sizeof r->def, "Jam01");
        return 1;
    }
    if (dlg == 138 && (id == 1414 || id == 1584 || id == 1103 || id == 1104)) {   /* GPxSAT: the page of the chosen mode */
        static const char *const sec[4] = { "Classic", "Digital", "Digital2004", "Digital2010" }, *const font[4] = { "manana", "futura", "digital2004", "din-bold" };
        static const char *const t1[4] = { "classic_m.tga", "digital_m.tga", "digital_2004_m.tga", "" }, *const t2[4] = { "classic_b.tga", "digital_b.tga", "digital_2004_b.tga", "" };
        int m = sel_of(p, 1342); if (m < 0 || m > 3) m = 3;
        snprintf(r->file, sizeof r->file, "gpxsat.ini");
        if (id == 1584) { r->kind = 't'; snprintf(r->sec, sizeof r->sec, "Font"); snprintf(r->key, sizeof r->key, "Font%s", sec[m]); snprintf(r->def, sizeof r->def, "%s", font[m]); return 1; }
        r->kind = 'e'; snprintf(r->sec, sizeof r->sec, "%s", sec[m]);
        if (id == 1414) { snprintf(r->key, sizeof r->key, "CarsPerPage"); snprintf(r->def, sizeof r->def, "%d", m == 1 ? 6 : 8); }
        else { snprintf(r->key, sizeof r->key, id == 1103 ? "Type1" : "Type2"); snprintf(r->def, sizeof r->def, "%s", id == 1103 ? t1[m] : t2[m]); }
        return 1;
    }
    if (dlg == 153 && id >= 1131 && id <= 1152) {   /* car set jams */
        r->kind = 'e'; snprintf(r->file, sizeof r->file, "%s", g_setfile); snprintf(r->sec, sizeof r->sec, "%s", CARSET_JAMS[g_cs_cat].sec); snprintf(r->key, sizeof r->key, "Jam%02d", id - 1131 + 1); r->def[0] = 0;
        return 1;
    }
    if (dlg == 133 && id >= 1015 && id <= 1031 && g_multi) {   /* "Multi...": a performance file per track */
        r->kind = 'e'; snprintf(r->file, sizeof r->file, "gpxset.ini"); snprintf(r->sec, sizeof r->sec, "PerfManager"); snprintf(r->key, sizeof r->key, "Path%02d", id - 1015 + 1); r->def[0] = 0;
        return 1;
    }
    if (dlg == 133 && id >= 1015 && id <= 1031) {   /* the 17 tracks of a track set */
        r->kind = 'e'; snprintf(r->file, sizeof r->file, "%s", g_setfile); snprintf(r->sec, sizeof r->sec, "TrackFiles"); snprintf(r->key, sizeof r->key, "f1ct%02d", id - 1015 + 1);
        snprintf(r->def, sizeof r->def, "Circuits\\F1CT%02d.dat", id - 1015 + 1);   /* no set file (or no line): the game's own tracks, as GPxPatch shows them */
        return 1;
    }
    return 0;
}
static void load_values(page_t *p) {
    for (int i = 0; i < p->g.n && i < MAXC; i++) {
        rb_t b; if (!resolve(p, i, &b)) continue;
        const char *v = ini_get(b.file, b.sec, b.key, b.def);
        if (b.kind == 'k') { p->v[i].iscolor = 1; p->v[i].ival = (int)strtoul(v, NULL, 0); snprintf(p->v[i].sval, sizeof p->v[i].sval, "%s", v); continue; }
        if (b.kind == 'm') { p->v[i].multi = 1; int m = 0; char t2[256]; snprintf(t2, sizeof t2, "%s", v); for (char *tok = strtok(t2, ","); tok; tok = strtok(NULL, ",")) { int bit = atoi(tok); if (bit >= 0 && bit < 31) m |= 1 << bit; } p->v[i].ival = m; continue; }
        if (b.kind == 'e' || b.kind == 't') snprintf(p->v[i].sval, sizeof p->v[i].sval, "%s", v);
        else if (b.kind == 'r') p->v[i].ival = !strcasecmp(v, p->g.c[i].id == 1241 ? "Jam01" : "Jam02");
        else p->v[i].ival = atoi(v);
        if (b.kind == 't') for (int k = 0; k < p->v[i].nitems; k++) if (!strcasecmp(p->v[i].items[k], v)) { snprintf(p->v[i].sval, sizeof p->v[i].sval, "%s", p->v[i].items[k]); }
    }
}
static void save_value(page_t *p, int i) {
    rb_t b; if (!resolve(p, i, &b)) return;
    char v[600];
    if (b.kind == 'k') { int hex = !strncasecmp(p->v[i].sval, "0x", 2) || !p->v[i].sval[0]; if (hex) snprintf(v, sizeof v, "0x%08x", (unsigned)p->v[i].ival); else snprintf(v, sizeof v, "%u", (unsigned)p->v[i].ival); snprintf(p->v[i].sval, sizeof p->v[i].sval, "%s", v); }
    else if (b.kind == 'm') { v[0] = 0; for (int bit = 0; bit < 31; bit++) if (p->v[i].ival & (1 << bit)) { size_t l = strlen(v); snprintf(v + l, sizeof v - l, "%s%d", l ? "," : "", bit); } }
    else if (b.kind == 'r') { if (!p->v[i].ival) return; snprintf(v, sizeof v, "%s", p->g.c[i].id == 1241 ? "Jam01" : "Jam02"); }
    else if (b.kind == 'e' || b.kind == 't') snprintf(v, sizeof v, "%s", p->v[i].sval); else snprintf(v, sizeof v, "%d", p->v[i].ival);
    ini_put(b.file, b.sec, b.key, v);
    /* the Linux runtime reads two of them from gp3rt.ini */
    if (p->g.id == 129 && p->g.c[i].id == 1060) ini_put("gp3rt.ini", "game", "intro_videos", p->v[i].ival ? "0" : "1");
}
/* what GPxPatch shows or greys out by itself */
static void page_rules(page_t *p) {
    int i;
    if (p->g.id == 131) {   /* GPxLap: two options exist only in GP3 1.13, two only in GP3 2000 */
        static const int only113[] = { 1004, 1005 }, only2000[] = { 1607, 1574 };
        for (int k = 0; k < 2; k++) { if ((i = idx_of(p, only113[k])) >= 0) p->hidden[i] = g_is2000; if ((i = idx_of(p, only2000[k])) >= 0) p->hidden[i] = !g_is2000; }
    }
    if (p->g.id == 158) {   /* car set per track only with "track specific" on */
        int ts = sel_of(p, 1250); if ((i = idx_of(p, 1204)) >= 0) p->disabled[i] = !ts; if (!ts && i >= 0 && p->v[i].ival) { p->v[i].ival = 0; }
    }
    if (p->g.id == 161) {   /* the item fields wait for an item */
        static const int item_ids[] = { 1387, 1605, 1596, 1433, 1385, 1386, 1626, 1615, 1438, 1439, 1388, 1448, 1444, 1447, 1449, 1450, 1459, 1460, 1435, 1437, 1565, 1568, 1569, 1570 };
        for (size_t k = 0; k < sizeof item_ids / sizeof *item_ids; k++) if ((i = idx_of(p, item_ids[k])) >= 0) p->disabled[i] = !g_pnl_sec[0];
        if ((i = idx_of(p, 1460)) >= 0 && g_pnl_sec[0]) p->disabled[i] = sel_of(p, 1459) == 0;
    }
    if (p->g.id == 129) {   /* the debug details follow the "debug mode" box */
        int dbg = (i = idx_of(p, 1002)) >= 0 ? p->v[i].ival : 0;
        if ((i = idx_of(p, 1003)) >= 0) p->disabled[i] = !dbg;
        if ((i = idx_of(p, 1059)) >= 0) p->disabled[i] = !dbg;
    }
}
static char g_font_sec[128];          /* the font chosen in the Fonts window */
static char g_fontsfile[300] = "fonts.ini";   /* fonts.ini, or the local one of a GPxCInfo panel */
static int g_key_capture = -1;        /* the Keys window line waiting for a key */
static void fonts_list(page_t *p); static void font_fields(page_t *p, int save); static void keys_list(page_t *p);
static void ask_name(const char *title, void (*done)(const char *)); static void font_added(const char *name); static void ini_drop_section(const char *file, const char *sec);
static void open_url(const char *path);
static void layout(void);
static void track_labels(page_t *p);
static void pnl_sub(modal_t *m); static void pnl_open(const char *path); static void pnl_choose_type(void);
static void open_modal(int id); static void close_modal(int ok); static void msgbox(const char *msg, const char *extra); static void open_full_screen(void); static void draw_modal(void); static void browse(page_t *p, const browse_t *b);
/* GPxSAT: GPxPatch puts the page of the chosen mode (dialog 198, one per mode) into the hidden frame 1588 */
static int g_sat_base = -1;
static void sat_page(page_t *p) {
    if (p->g.id != 138) return;
    if (g_sat_base < 0) g_sat_base = p->g.n;
    for (int i = g_sat_base; i < p->g.n && i < MAXC; i++) { free_items(&p->v[i]); memset(&p->v[i], 0, sizeof p->v[i]); p->hidden[i] = p->disabled[i] = 0; }
    p->g.n = g_sat_base;
    int ci = idx_of(p, 1588); dlg_t d; if (ci < 0 || !get_dlg(198, &d)) return;
    p->g.c = realloc(p->g.c, sizeof(ctl_t) * (size_t)(p->g.n + d.n));
    for (int k = 0; k < d.n && p->g.n < MAXC; k++) { ctl_t c = d.c[k]; c.x += p->g.c[ci].x; c.y += p->g.c[ci].y; p->g.c[p->g.n++] = c; }
    dlg_free(&d);
    int i;
    if ((i = idx_of(p, 1584)) >= 0) { char fp[PATH_MAX]; size_t n; char *t = lx_file_path("fonts.ini", fp, sizeof fp) ? lx_read_file(fp, &n) : NULL;
        for (char *q = t; q && *q; ) { char *nl = strchr(q, '\n'); if (*q == '[') { char *e = strchr(q, ']'); if (e && (!nl || e < nl)) { char sec[128]; size_t l = (size_t)(e - q - 1) < sizeof sec - 1 ? (size_t)(e - q - 1) : sizeof sec - 1; memcpy(sec, q + 1, l); sec[l] = 0; add_item(&p->v[i], sec); } } q = nl ? nl + 1 : NULL; }
        free(t); }
    int dig2010 = sel_of(p, 1342) == 3;   /* Digital 2010 has no tyre bitmaps */
    static const int tyre[] = { 1103, 1104, 1107, 1096 };
    for (int k = 0; k < 4; k++) if ((i = idx_of(p, tyre[k])) >= 0) p->disabled[i] = dig2010;
}
static void open_page(int tab) {
    page_clear(&g_page); g_tab = tab; g_sat_base = -1;
    if (!get_dlg(TABS[tab].dlg, &g_page.g)) return;
    dlginit(&g_page); fill_lists(&g_page); load_values(&g_page); sat_page(&g_page); load_values(&g_page); page_rules(&g_page); track_labels(&g_page);
    layout();
    ini_put("gp3rt.ini", "gpxpatch", "tab", TABS[tab].name);
}
static int tab_rect(int i, SDL_Rect *r);
static void open_main(void) {
    page_clear(&g_main);
    if (!get_dlg(102, &g_main.g)) return;
    dlginit(&g_main); fill_lists(&g_main);
    SDL_SetWindowTitle(g_win, g_main.g.caption[0] ? g_main.g.caption : "GPxPatch");
    int ww = DX(g_main.g.w), wh = DY(g_main.g.h);
    SDL_SetWindowSize(g_win, (int)(ww / g_hidpi), (int)(wh / g_hidpi));
    if (!(SDL_GetWindowFlags(g_win) & SDL_WINDOW_SHOWN)) SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    open_page(g_tab);
}
/* the page sits in the tab control's display area */
static int g_tabx, g_taby, g_tabw, g_tabh, g_tabhead;
static void layout(void) {
    int t = idx_of(&g_main, 1001); if (t < 0) return;
    ctl_t *c = &g_main.g.c[t];
    g_tabx = DX(c->x); g_taby = DY(c->y); g_tabw = DX(c->w); g_tabh = DY(c->h);
    g_tabhead = line_h() + 6 * S;
    g_page.ox = g_tabx + 2 * S; g_page.oy = g_taby + g_tabhead + 2 * S;
}

/* ------------------------------------------------------------------ */
/* controls                                                             */
static void crect(page_t *p, int i, SDL_Rect *r) {
    ctl_t *c = &p->g.c[i];
    r->x = p->ox + DX(c->x); r->y = p->oy + DY(c->y); r->w = DX(c->x + c->w) - DX(c->x); r->h = DY(c->y + c->h) - DY(c->y);
    if (c->kind == K_COMBO) r->h = line_h() + 8 * S;   /* closed combo box (the template height is the list) */
}
static int enabled(page_t *p, int i) { return !(p->g.c[i].style & WS_DISABLED) && !p->disabled[i]; }
static int visible(page_t *p, int i) { return (p->g.c[i].style & WS_VISIBLE) && !p->hidden[i]; }
static int btn_type(ctl_t *c) { return (int)(c->style & 0x0f); }
static const char *combo_text(page_t *p, int i) {
    const bind_t *b = find_bind(p->g.id, p->g.c[i].id); const pbind_t *pb = g_pnl[0] ? find_pbind(p->g.id, p->g.c[i].id) : NULL;
    int drop_edit = (p->g.c[i].style & 3) == 2;   /* CBS_DROPDOWN: the text is the value */
    if ((b && b->kind == 't') || (pb && pb->kind == 't') || drop_edit || (p->g.id == 138 && p->g.c[i].id == 1584)) return p->v[i].sval;
    val_t *v = &p->v[i]; return v->ival >= 0 && v->ival < v->nitems ? v->items[v->ival] : "";
}
static void draw_ctl(page_t *p, int pg, int i) {
    ctl_t *c = &p->g.c[i]; if (!visible(p, i)) return;
    SDL_Rect r; crect(p, i, &r); int en = enabled(p, i), lh = line_h();
    int pressed = g_press_pg == pg && g_press == i && g_hover_in;
    switch (c->kind) {
    case K_STATIC: {
        int ss = (int)(c->style & 0x1f);
        if (ss == 0x10 || ss == 0x11 || ss == 0x12) { if (ss == 0x11) vline(r.x, r.y, r.h, C_SH), vline(r.x + T, r.y, r.h, C_HI); else hline(r.x, r.y, r.w, C_SH), hline(r.x, r.y + T, r.w, C_HI); break; }   /* etched lines */
        if (ss == 0x07 || ss == 0x08 || ss == 0x09) { if (ss == 0x08) fill(r.x, r.y, r.w, r.h, C_SH); else frame(r.x, r.y, r.w, r.h, ss == 0x07 ? C_DK : C_SH, ss == 0x07 ? C_DK : C_SH); break; }
        if (ss == 0x03 || ss == 0x0e) break;   /* icon / bitmap */
        if (ss == 0x0b) { text_state(r.x, r.y, c->text, -1, en); break; }   /* SS_LEFTNOWORDWRAP */
        if (r.h < 2 * lh && !strchr(c->text, '\n')) {   /* one-line label: like Windows, cut at the control's edge */
            SDL_Rect clip = { r.x, r.y - S, r.w + 2 * S, r.h + 2 * S }; SDL_RenderSetClipRect(g_ren, &clip);
            int tw = text_w(c->text, -1); text_state(ss == 1 ? r.x + (r.w - tw) / 2 : ss == 2 ? r.x + r.w - tw : r.x, r.y, c->text, -1, en);
            SDL_RenderSetClipRect(g_ren, NULL); break;
        }
        text_box(r.x, r.y, r.w, r.h, c->text, ss == 1 ? 1 : ss == 2 ? 2 : 0, en, 1);
        break; }
    case K_BUTTON: {
        int bt = btn_type(c);
        if (bt == 7) {   /* group box: etched frame, text on top */
            int tw = c->text[0] ? text_w(c->text, -1) + 4 * S : 0, ty = r.y + lh / 2;
            frame(r.x, ty, r.w, r.h - lh / 2, C_SH, C_HI); frame(r.x + T, ty + T, r.w - 2 * T, r.h - lh / 2 - 2 * T, C_HI, C_SH);
            if (tw) { fill(r.x + 6 * S, r.y, tw, lh, C_FACE); text_state(r.x + 8 * S, r.y, c->text, -1, en); }
            break;
        }
        if (bt == 2 || bt == 3 || bt == 5 || bt == 6) {   /* check boxes */
            int bs = 13 * S, by = r.y + (r.h - bs) / 2, v = p->v[i].ival;
            { int tw = text_w(c->text, -1) + 18 * S; fill(r.x, r.y, tw < r.w ? tw : r.w, r.h, C_FACE); }   /* a check box paints its own background (over a group frame) */
            sunken(r.x, by, bs, bs); fill(r.x + 2 * T, by + 2 * T, bs - 4 * T, bs - 4 * T, (pressed || !en) ? C_FACE : C_WIN);
            if (v == 2 && (bt == 5 || bt == 6)) { for (int yy = 0; yy < 9; yy++) for (int xx = 0; xx < 9; xx++) if ((xx + yy) & 1) fill(r.x + (2 + xx) * S, by + (2 + yy) * S, S, S, C_FACE); art(r.x + 3 * S, by + 3 * S, CHECK, 7, C_GRAY); }
            else if (v) art(r.x + 3 * S, by + 3 * S, CHECK, 7, en ? C_TXT : C_GRAY);
            text_box(r.x + 16 * S, r.y + (r.h - lh) / 2, r.w - 16 * S, r.h, c->text, 0, en, 1);
            break;
        }
        if ((c->style & 0x1000) && (bt == 2 || bt == 3 || bt == 4 || bt == 9)) {   /* BS_PUSHLIKE: a button that stays down */
            int on = p->v[i].ival; fill(r.x, r.y, r.w, r.h, on ? C_HI : C_FACE);
            if (on || pressed) sunken(r.x, r.y, r.w, r.h); else raised(r.x, r.y, r.w, r.h);
            int tw = text_w(c->text, -1), o = on ? S : 0; text_state(r.x + (r.w - tw) / 2 + o, r.y + (r.h - lh) / 2 + o, c->text, -1, en);
            break;
        }
        if (bt == 4 || bt == 9) {   /* radio */
            int bs = 12 * S, by = r.y + (r.h - bs) / 2, v = p->v[i].ival;
            static const char *const OUT1[] = { "    ####    ", "  ##    ##  ", " #        # ", " #        # ", "#          #", "#          #", "#          #", "#          #", " #        # ", " #        # ", "  ##    ##  ", "    ####    " };
            fill(r.x + 2 * S, by + 2 * S, 8 * S, 8 * S, C_WIN); fill(r.x + 1 * S, by + 4 * S, 10 * S, 4 * S, C_WIN); fill(r.x + 4 * S, by + 1 * S, 4 * S, 10 * S, C_WIN);
            art(r.x, by, OUT1, 12, C_SH);
            if (v) { fill(r.x + 4 * S, by + 4 * S, 4 * S, 4 * S, C_TXT); fill(r.x + 5 * S, by + 3 * S, 2 * S, 6 * S, C_TXT); fill(r.x + 3 * S, by + 5 * S, 6 * S, 2 * S, C_TXT); }
            text_box(r.x + 16 * S, r.y + (r.h - lh) / 2, r.w - 16 * S, r.h, c->text, 0, en, 1);
            (void)RADIO_OUT_A;
            break;
        }
        /* push buttons (also the small "..." browse buttons) */
        int def = bt == 1;
        fill(r.x, r.y, r.w, r.h, C_FACE);
        if (pressed) pressed_btn(r.x, r.y, r.w, r.h);
        else if (def) { frame(r.x, r.y, r.w, r.h, C_DK, C_DK); raised(r.x + T, r.y + T, r.w - 2 * T, r.h - 2 * T); }
        else raised(r.x, r.y, r.w, r.h);
        int o = pressed ? S : 0;
        if (p->v[i].iscolor) {   /* colour button: the colour itself */
            unsigned long cv = (unsigned long)(unsigned)p->v[i].ival; int m2 = 4 * S;
            fill(r.x + m2 + o, r.y + m2 + o, r.w - 2 * m2, r.h - 2 * m2, en ? (col_t){ (Uint8)(cv >> 16), (Uint8)(cv >> 8), (Uint8)cv } : C_FACE);
            frame(r.x + m2 + o, r.y + m2 + o, r.w - 2 * m2, r.h - 2 * m2, en ? C_TXT : C_SH, en ? C_TXT : C_SH);
            break;
        }
        if (c->style & 0x80) {   /* BS_BITMAP */
            SDL_Texture *bm = bitmap(button_bitmap(p->g.id, c->id)); int bw2 = 16 * S, bh2 = 15 * S;
            if (bm) { if (!en) SDL_SetTextureAlphaMod(bm, 110); SDL_Rect dst = { r.x + (r.w - bw2) / 2 + o, r.y + (r.h - bh2) / 2 + o, bw2, bh2 }; SDL_RenderCopy(g_ren, bm, NULL, &dst); SDL_SetTextureAlphaMod(bm, 255); break; }
        }
        const char *t = c->text[0] && !(c->style & 0x80) ? c->text : "...";
        int tw = text_w(t, -1);
        if (tw <= r.w - 4 * S) text_state(r.x + (r.w - tw) / 2 + o, r.y + (r.h - lh) / 2 + o, t, -1, en);
        else text_box(r.x + 3 * S + o, r.y + 2 * S + o, r.w - 6 * S, r.h, t, 1, en, 1);
        break; }
    case K_EDIT: {
        sunken(r.x, r.y, r.w, r.h); fill(r.x + 2 * T, r.y + 2 * T, r.w - 4 * T, r.h - 4 * T, en ? C_WIN : C_FACE);
        const char *t = p->v[i].sval; int tx = r.x + 4 * S, ty = r.y + (r.h - lh) / 2;
        SDL_Rect clip = { r.x + 2 * T, r.y + 2 * T, r.w - 4 * T, r.h - 4 * T }; SDL_RenderSetClipRect(g_ren, &clip);
        if (c->style & 0x20) { char st[512]; size_t n = strlen(t); for (size_t k = 0; k < n && k < 511; k++) st[k] = '*'; st[n < 511 ? n : 511] = 0; text(tx, ty, st, -1, en ? C_TXT : C_GRAY); }
        else { int sh = 0; if (g_focus_pg == pg && g_focus == i) { int cw = text_w(t, g_caret); if (cw > r.w - 10 * S) sh = cw - (r.w - 10 * S); }
            text(tx - sh, ty, t, -1, en ? C_TXT : C_GRAY);
            if (g_focus_pg == pg && g_focus == i && (SDL_GetTicks() / 530) % 2 == 0) fill(tx - sh + text_w(t, g_caret), ty, S > 1 ? S / 2 + 1 : 1, lh, C_TXT); }
        SDL_RenderSetClipRect(g_ren, NULL);
        break; }
    case K_COMBO: {
        int bw = 16 * S;
        sunken(r.x, r.y, r.w, r.h); fill(r.x + 2 * T, r.y + 2 * T, r.w - 4 * T, r.h - 4 * T, en ? C_WIN : C_FACE);
        int bx = r.x + r.w - 2 * T - bw, byy = r.y + 2 * T, bh = r.h - 4 * T, open = g_drop_pg == pg && g_drop == i;
        fill(bx, byy, bw, bh, C_FACE);
        if (open) frame(bx, byy, bw, bh, C_SH, C_SH); else raised(bx, byy, bw, bh);
        art(bx + (bw - 7 * S) / 2 + (open ? S : 0), byy + (bh - 4 * S) / 2 + (open ? S : 0), ARROW, 4, en ? C_TXT : C_GRAY);
        SDL_Rect clip = { r.x + 2 * T, r.y + 2 * T, r.w - 4 * T - bw, r.h - 4 * T }; SDL_RenderSetClipRect(g_ren, &clip);
        const char *t = combo_text(p, i); int ty = r.y + (r.h - lh) / 2;
        int focus = g_focus_pg == pg && g_focus == i;
        if ((c->style & 3) == 3 && focus) { fill(r.x + 3 * S, r.y + 3 * S, r.w - 6 * S - bw, r.h - 6 * S, C_SEL); text(r.x + 4 * S, ty, t, -1, C_SELT); }
        else text(r.x + 4 * S, ty, t, -1, en ? C_TXT : C_GRAY);
        if ((c->style & 3) == 2 && focus && (SDL_GetTicks() / 530) % 2 == 0) fill(r.x + 4 * S + text_w(t, g_caret), ty, S / 2 + 1, lh, C_TXT);
        SDL_RenderSetClipRect(g_ren, NULL);
        break; }
    case K_LISTVIEW: {   /* report view: action | key (keys.ini) */
        sunken(r.x, r.y, r.w, r.h); fill(r.x + 2 * T, r.y + 2 * T, r.w - 4 * T, r.h - 4 * T, C_WIN);
        val_t *v = &p->v[i]; int ih = lh + 2 * S, hh = lh + 4 * S, cw0 = (r.w - 4 * T) * 62 / 100, x0 = r.x + 2 * T, y0 = r.y + 2 * T;
        fill(x0, y0, r.w - 4 * T, hh, C_FACE); raised(x0, y0, cw0, hh); raised(x0 + cw0, y0, r.w - 4 * T - cw0, hh);
        text(x0 + 4 * S, y0 + 2 * S, "Function", -1, C_TXT); text(x0 + cw0 + 4 * S, y0 + 2 * S, "Key", -1, C_TXT);
        int vis = (r.h - 4 * T - hh) / ih; if (v->sel_scroll > v->nitems - vis) v->sel_scroll = v->nitems - vis > 0 ? v->nitems - vis : 0;
        SDL_Rect clip = { x0, y0 + hh, r.w - 4 * T, r.h - 4 * T - hh }; SDL_RenderSetClipRect(g_ren, &clip);
        for (int k = v->sel_scroll; k < v->nitems; k++) {
            int yy = y0 + hh + (k - v->sel_scroll) * ih; char a[128]; snprintf(a, sizeof a, "%s", v->items[k]); char *eq = strchr(a, '='); const char *kv = "";
            if (eq) { *eq = 0; kv = eq + 1; }
            if (k == g_key_capture) kv = "...";
            int sel = k == v->ival; col_t tc = sel ? C_SELT : C_TXT;
            if (sel) fill(x0, yy, r.w - 4 * T, ih, C_SEL);
            text(x0 + 4 * S, yy + S, a, -1, tc); text(x0 + cw0 + 4 * S, yy + S, kv, -1, tc);
        }
        SDL_RenderSetClipRect(g_ren, NULL);
        break; }
    case K_LIST: {
        sunken(r.x, r.y, r.w, r.h); fill(r.x + 2 * T, r.y + 2 * T, r.w - 4 * T, r.h - 4 * T, C_WIN);
        val_t *v = &p->v[i]; int ih = lh + 2 * S;
        SDL_Rect clip = { r.x + 2 * T, r.y + 2 * T, r.w - 4 * T, r.h - 4 * T }; SDL_RenderSetClipRect(g_ren, &clip);
        int chk = p->g.id == 139 && c->id == 1621;   /* GPxCInfo: a check box per panel */
        if (chk) ih = 15 * S;
        int vis = (r.h - 4 * S) / ih, sbw = 0;
        if (v->sel_scroll > v->nitems - vis) v->sel_scroll = v->nitems - vis; if (v->sel_scroll < 0) v->sel_scroll = 0;
        if (v->nitems > vis) {   /* scroll bar */
            sbw = 16 * S; int sx = r.x + r.w - 2 * T - sbw, sy = r.y + 2 * T, sh = r.h - 4 * T;
            for (int yy = 0; yy < sh; yy += T) for (int xx = (yy / T) & 1; xx < sbw; xx += 2 * T) fill(sx + xx, sy + yy, T, T, C_HI);
            fill(sx, sy, sbw, sbw, C_FACE); raised(sx, sy, sbw, sbw); fill(sx, sy + sh - sbw, sbw, sbw, C_FACE); raised(sx, sy + sh - sbw, sbw, sbw);
            static const char *const UP[] = { "   #   ", "  ###  ", " ##### ", "#######" };
            art(sx + (sbw - 7 * S) / 2, sy + (sbw - 4 * S) / 2, UP, 4, C_TXT); art(sx + (sbw - 7 * S) / 2, sy + sh - sbw + (sbw - 4 * S) / 2, ARROW, 4, C_TXT);
            int track = sh - 2 * sbw, th = track * vis / v->nitems; if (th < 8 * S) th = 8 * S;
            int ty = sy + sbw + (track - th) * v->sel_scroll / (v->nitems - vis);
            fill(sx, ty, sbw, th, C_FACE); raised(sx, ty, sbw, th);
            clip.w -= sbw; SDL_RenderSetClipRect(g_ren, &clip);
        }
        for (int k = v->sel_scroll; k < v->nitems; k++) {
            int yy = r.y + 2 * S + (k - v->sel_scroll) * ih, tx = r.x + 4 * S; const char *t = v->items[k];
            if (chk) { int bs = 13 * S; sunken(r.x + 3 * S, yy + S, bs, bs); fill(r.x + 5 * S, yy + 3 * S, bs - 4 * S, bs - 4 * S, C_WIN); if (t[0] == '1') art(r.x + 6 * S, yy + 4 * S, CHECK, 7, C_TXT); t++; tx = r.x + 20 * S; }
            if (v->multi ? (v->ival >> k) & 1 : k == v->ival) { fill(tx - S, yy, r.w - (tx - r.x) - S - sbw, ih, C_SEL); text(tx, yy + (ih - lh) / 2, t, -1, C_SELT); } else text(tx, yy + (ih - lh) / 2, t, -1, C_TXT);
        }
        SDL_RenderSetClipRect(g_ren, NULL);
        break; }
    case K_TAB: break;   /* drawn by draw_tabs */
    default: break;
    }
}
/* the tab control of the main window */
static int tab_rect(int i, SDL_Rect *r) {
    int x = g_tabx + 2 * S;
    for (int k = 0; k < NTABS; k++) {
        int w = text_w(TABS[k].name, -1) + 12 * S;
        if (k == i) { r->x = x; r->y = g_taby + 2 * S; r->w = w; r->h = g_tabhead - 2 * S; return 1; }
        x += w;
    }
    return 0;
}
static void draw_tabs(void) {
    /* body */
    int by = g_taby + g_tabhead;
    fill(g_tabx, by, g_tabw, g_tabh - g_tabhead, C_FACE);
    frame(g_tabx, by, g_tabw, g_tabh - g_tabhead, C_HI, C_DK); frame(g_tabx + T, by + T, g_tabw - 2 * T, g_tabh - g_tabhead - 2 * T, C_FACE, C_SH);
    for (int k = 0; k < NTABS; k++) {
        SDL_Rect r; tab_rect(k, &r); int sel = k == g_tab;
        if (sel) { r.x -= 2 * S; r.w += 4 * S; r.y -= 2 * S; r.h += 2 * S + S; }
        fill(r.x, r.y, r.w, r.h, C_FACE);
        vline(r.x, r.y + 2 * T, r.h - 2 * T, C_HI); fill(r.x + T, r.y + T, T, T, C_HI); hline(r.x + 2 * T, r.y, r.w - 4 * T, C_HI);
        fill(r.x + r.w - 2 * T, r.y + T, T, T, C_DK); vline(r.x + r.w - T, r.y + 2 * T, r.h - 2 * T, C_DK); vline(r.x + r.w - 2 * T, r.y + 2 * T, r.h - 2 * T, C_SH);
        if (sel) fill(r.x + T, by, r.w - 2 * T, T * 2, C_FACE);
        text(r.x + (r.w - text_w(TABS[k].name, -1)) / 2, r.y + (sel ? 3 * S : 3 * S), TABS[k].name, -1, C_TXT);
    }
}
static SDL_Rect drop_rect(page_t *p, int i) {
    SDL_Rect r; crect(p, i, &r); val_t *v = &p->v[i];
    int ih = line_h() + 2 * S, n = v->nitems ? v->nitems : 1, maxh = DY(p->g.c[i].h) - r.h; if (maxh < ih * 3) maxh = ih * 8;
    int h = n * ih + 2 * S; if (h > maxh) h = (maxh / ih) * ih + 2 * S;
    SDL_Rect d = { r.x, r.y + r.h, r.w, h };
    int ww, wh; SDL_GetRendererOutputSize(g_ren, &ww, &wh); if (d.y + d.h > wh) d.y = r.y - d.h;
    return d;
}
static void draw_drop(void) {
    if (g_drop < 0) return;
    page_t *p = PG(g_drop_pg); val_t *v = &p->v[g_drop];
    SDL_Rect d = drop_rect(p, g_drop); int ih = line_h() + 2 * S, vis = (d.h - 2 * S) / ih;
    fill(d.x, d.y, d.w, d.h, C_WIN); frame(d.x, d.y, d.w, d.h, C_TXT, C_TXT);
    for (int k = 0; k < vis && k + g_drop_scroll < v->nitems; k++) {
        int it = k + g_drop_scroll, yy = d.y + S + k * ih;
        if (it == g_drop_hover) { fill(d.x + S, yy, d.w - 2 * S, ih, C_SEL); text(d.x + 3 * S, yy + S, v->items[it], -1, C_SELT); }
        else text(d.x + 3 * S, yy + S, v->items[it], -1, C_TXT);
    }
}
static void draw_all(void) {
    int ww, wh; SDL_GetRendererOutputSize(g_ren, &ww, &wh);
    fill(0, 0, ww, wh, C_FACE);
    layout();
    draw_tabs();
    for (int i = 0; i < g_main.g.n; i++) draw_ctl(&g_main, 0, i);
    for (int i = 0; i < g_page.g.n; i++) draw_ctl(&g_page, 1, i);
    draw_modal();
    draw_drop();
}

/* ------------------------------------------------------------------ */
/* actions                                                              */
static void open_url(const char *path) {
    pid_t p = fork();
    if (p == 0) { int nul = open("/dev/null", 2); if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); } setsid(); execlp("xdg-open", "xdg-open", path, (char *)NULL); _exit(127); }
}
static void help(void) {
    char docs[PATH_MAX], f[PATH_MAX];
    if (!lx_ci_walk(g_gpxdir, "docs", docs, sizeof docs)) return;
    const char *code = g_nlang ? g_langs[g_lang].code : "EN";
    char rel[16]; snprintf(rel, sizeof rel, "%s/index.html", code);
    if (strcmp(code, "EN") && lx_ci_walk(docs, rel, f, sizeof f)) { open_url(f); return; }
    if (lx_ci_walk(docs, "index.html", f, sizeof f)) open_url(f);
}
static int g_running; static Uint32 g_hide_at; static int g_hidden;
/* "Skip intro" of GPxPatch is also what GP3RT follows (gp3rt.ini [game] intro_videos) */
static void intro_sync(void) {
    int skip = atoi(ini_get("gpxpatch.ini", "Game", "SkipIntro", "0"));
    if (atoi(ini_get("gp3rt.ini", "game", "intro_videos", "1")) != !skip) ini_put("gp3rt.ini", "game", "intro_videos", skip ? "0" : "1");
}
static void command(page_t *p, int pg, int i) {
    int id = p->g.c[i].id;
    if (pg == 0) {
        if (id == 1000) { if (!g_running) { intro_sync(); lx_start_game(1); g_running = 1; g_hide_at = SDL_GetTicks() + 1500; } return; }   /* Start GPx: the window steps aside while the game runs */
        if (id == 1 || id == 2) { g_quit = 1; return; }                                                              /* Exit */
        if (id == 9) { help(); return; }                                                                               /* Help */
    }
    if (pg >= 2) {
        if (id == 3000) { close_modal(0); open_full_screen(); return; }
        if (id == 1 || id == 2) { close_modal(id == 1 && (p->g.id > 0 || p->g.id == -3 || p->g.id == -4)); return; }
    }
    if (p->g.id == 133 && id >= 1033 && id <= 1049) { browse_t b = { 133, id, 1015 + (id - 1033), 'g', g_multi ? "txt" : "dat" }; browse(p, &b); return; }
    if (p->g.id == 153 && id >= 1155 && id <= 1176) { browse_t b = { 153, id, 1131 + (id - 1155), 'l', "jam" }; browse(p, &b); return; }
    if (p->g.id == 154 && (id == 1282 || id == 1283)) { pitorder_move(p, id == 1282 ? -1 : 1); return; }
    if (p->g.id == 158 && id == 1181) { int k = idx_of(p, 1180); if (k < 0 || !p->v[k].sval[0]) return; snprintf(g_setfile, sizeof g_setfile, "%s", p->v[k].sval); open_modal(144); return; }
    if (p->g.id == 145 && id == 1236) { g_multi = 1; open_modal(133); return; }
    if (p->v[i].iscolor) { unsigned long cv = (unsigned long)(unsigned)p->v[i].ival; if (color_pick(&cv)) { p->v[i].ival = (int)cv; changed(p, pg, i); } return; }
    if (p->g.id == 139) {   /* GPxCInfo panels */
        int li = idx_of(p, 1621), sel = li >= 0 ? p->v[li].ival : -1, n = cinfo_count();
        if (id == 1366) {   /* add a panel file from gpxpdata\pnl */
            char root[PATH_MAX], f[PATH_MAX]; if (!lx_ci_walk(g_gpxdir, "pnl", root, sizeof root)) snprintf(root, sizeof root, "%s", g_gpxdir);
            if (!pick_file(root, "ini", f, sizeof f)) return;
            size_t rl = strlen(root); if (strncmp(f, root, rl) || f[rl] != '/') { msgbox("O painel precisa estar dentro de gpxpdata/pnl.\n\nThe panel must be inside gpxpdata/pnl.", NULL); return; }
            char v[PATH_MAX], kp[16], ke[16]; snprintf(v, sizeof v, "%s", f + rl + 1); for (char *q = v; *q; q++) if (*q == '/') *q = '\\';
            snprintf(kp, sizeof kp, "Path%d", n + 1); snprintf(ke, sizeof ke, "Enable%d", n + 1); ini_put("gpxcinfo.ini", "Panel", kp, v); ini_put("gpxcinfo.ini", "Panel", ke, "1");
            free_items(&p->v[li]); fill_lists(p); p->v[li].ival = n; return;
        }
        if (id == 1624 && sel >= 0) { cinfo_remove(sel); free_items(&p->v[li]); fill_lists(p); p->v[li].ival = sel < n - 1 ? sel : n - 2; return; }
        if ((id == 1622 || id == 1623) && sel >= 0) { int to = sel + (id == 1622 ? -1 : 1); if (to < 0 || to >= n) return; cinfo_swap(sel, to); free_items(&p->v[li]); fill_lists(p); p->v[li].ival = to; return; }
        if (id == 1624 || id == 1622 || id == 1623) return;
    }
    if (p->g.id == 161) {   /* panel editor */
        if (id == 1567) { pnl_choose_type(); return; }
        if (id == 1568) { pnl_delete(); pnl_list(p); load_values(p); page_rules(p); pnl_sub(&MT); return; }
        if (id == 1569 || id == 1570) { pnl_move(id == 1569 ? -1 : 1); pnl_list(p); return; }
        if (id == 1442) { open_modal(196); return; }
        if (id == 1460) { int t = sel_of(p, 1459); if (t == 1) open_modal(179); else if (t == 2) open_modal(190); return; }
        if (id == 1390) { char d[300]; pnl_dir(d, sizeof d); snprintf(g_fontsfile, sizeof g_fontsfile, "%s\\fonts.ini", d); open_modal(167); return; }
    }
    if (p->g.id == 190 && id == 1483) { browse_t b = { 190, 1483, 1482, 'n', "tga" }; browse(p, &b); return; }
    for (const browse_t *b = BROWSE; b->dlg; b++) if (b->dlg == p->g.id && b->btn == id) { browse(p, b); return; }
    if (p->g.id == 167) {   /* Fonts */
        if (id == 1472) { ask_name("Enter Name", font_added); return; }
        if (id == 1473 && g_font_sec[0]) { ini_drop_section(g_fontsfile, g_font_sec); g_font_sec[0] = 0; fonts_list(p); return; }
        if (id == 1404) return;   /* "Select...": the Windows font dialog; type the face name instead */
    }
    if (p->g.id == 158 && id == 1326) { int k = idx_of(p, 1324); snprintf(g_setfile, sizeof g_setfile, "%s", k >= 0 && p->v[k].sval[0] ? p->v[k].sval : "tracks.set"); open_modal(133); return; }
    if (p->g.id == 132 && id == 1128) { int k = idx_of(p, 1014); snprintf(g_setfile, sizeof g_setfile, "%s", k >= 0 && p->v[k].sval[0] ? p->v[k].sval : "tracks.set"); open_modal(133); return; }
    if (p->g.id == 140 && id == 1116) { int k = idx_of(p, 1111); char f[PATH_MAX], snd[PATH_MAX];   /* the sound set is a text file: open it */
        if (k >= 0 && lx_find_ci(g_gpxdir, "snd", snd, sizeof snd) && lx_ci_walk(snd, p->v[k].sval, f, sizeof f)) open_url(f); return; }
    if (p->g.id == 131 && id == 1604) {   /* ATL panel colour: the desktop's colour chooser */
        const char *cur = ini_get("gpxlap.ini", "ATL", "PanelColor", "0x00000000"); unsigned long n = strtoul(cur, NULL, 16);
        char cmd[256]; snprintf(cmd, sizeof cmd, "zenity --color-selection --title='GPxPatch' --color='rgb(%lu,%lu,%lu)' 2>/dev/null", (n >> 16) & 255, (n >> 8) & 255, n & 255);
        FILE *f = popen(cmd, "r"); char b[64] = ""; if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; pclose(f); }
        unsigned r, g, bl; if (sscanf(b, "rgb(%u,%u,%u)", &r, &g, &bl) == 3 || sscanf(b, "rgba(%u,%u,%u", &r, &g, &bl) == 3) {
            char v[16]; snprintf(v, sizeof v, "0x%08lx", (n & 0xff000000ul) | (unsigned long)(r << 16 | g << 8 | bl)); ini_put("gpxlap.ini", "ATL", "PanelColor", v); }
        else if (b[0] == '#' && strlen(b) >= 7) { unsigned long c = strtoul(b + 1, NULL, 16); char v[16]; snprintf(v, sizeof v, "0x%08lx", (n & 0xff000000ul) | (c & 0xffffff)); ini_put("gpxlap.ini", "ATL", "PanelColor", v); }
        return;
    }
    switch (id) {
    case 1235: if (p->g.id == 145) { open_modal(148); return; } break;   /* Season set editor */
    case 1415: open_modal(169); return;   /* Session settings */
    case 1188: open_modal(192); return;   /* Keys */
    case 1343: open_modal(167); return;   /* Fonts */
    case 1120: open_modal(143); return;   /* My IP */
    case 1573: open_modal(195); return;   /* Engine temperature */
    case 1530: open_modal(185); return;   /* RCR distances */
    case 1611: open_modal(205); return;   /* 3D sound settings */
    }
    hlog("gpxclassic: button %d on page %d (not yet available)", id, p->g.id);
    /* not made yet: offer the other screen (host/gpxui.c), which has every editor */
    msgbox(g_langs[g_lang].code[0] == 'P' && g_langs[g_lang].code[1] == 'T'
           ? "Esta janela ainda está sendo feita no visual original.\n\nA tela completa do GPxPatch para Linux já tem este editor."
           : "This window is still being made in the original look.\n\nThe full GPxPatch for Linux screen already has this editor.",
           g_langs[g_lang].code[0] == 'P' && g_langs[g_lang].code[1] == 'T' ? "Abrir a tela completa" : "Open the full screen");
}
static void open_full_screen(void) {
    char self[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) { self[n] = 0; pid_t q = fork(); if (q == 0) { setenv("GP3_GPXPATCH_MODERN", "1", 1); execl(self, self, lx_dir(), (char *)NULL); _exit(127); } }
}
static void changed(page_t *p, int pg, int i) {
    if (pg == 0 && p->g.c[i].id == 1327) {   /* language */
        int k = p->v[i].ival;
        if (k >= 0 && k < g_nlang && load_lang(k)) { ini_put("gp3rt.ini", "gpxpatch", "language", g_langs[k].code); open_main(); }
        return;
    }
    int id = p->g.c[i].id;
    if (pg >= 2) {
        modal_t *m = &g_ms[(pg - 2) / 2];
        if (p->g.id == 167) { if (id == 1400) { val_t *v = &p->v[i]; if (v->ival >= 0 && v->ival < v->nitems) snprintf(g_font_sec, sizeof g_font_sec, "%s", v->items[v->ival]); font_fields(p, 0); } else font_fields(p, 1); return; }
        if (p->g.id == 158 && id == 1204) { int k = idx_of(p, 1180); if (k >= 0) { rb_t b; resolve(p, k, &b); snprintf(p->v[k].sval, sizeof p->v[k].sval, "%s", ini_get(b.file, b.sec, b.key, b.def)); } page_rules(p); return; }
        if (p->g.id == 156 && id == 1236) { load_values(p); return; }
        if (p->g.id == 161 && id == 1384) { g_pnl_sel = p->v[i].ival; pnl_list(p); load_values(p); page_rules(p); pnl_sub(m); return; }
        if (p->g.id == 190 && id == 1553) { pnl_sub(m); return; }
        if (p->g.id == -4) return;
        if (p->g.id == 161 && id == 1605) { save_value(p, i); int k = idx_of(p, 1384); if (k >= 0) { pnl_list(p); } return; }
        if (m->immediate) save_value(p, i);   /* editors of a .set file write at once (Cancel restores the file) */
        page_rules(p); return;               /* the other dialogs: saved on OK */
    }
    save_value(p, i);
    if (p->g.id == 138 && id == 1342) { sat_page(p); load_values(p); }
    page_rules(p);
}

/* ------------------------------------------------------------------ */
/* dialogs opened by the buttons (OK / Cancel), drawn as Windows dialogs over the main window */
static int g_cap_h;
static void modal_geom(modal_t *m, int *x, int *y, int *w, int *h) {
    *w = DX(m->pg.g.w) + 6 * S; *h = DY(m->pg.g.h) + 6 * S + g_cap_h; *x = m->pg.ox - 3 * S; *y = m->pg.oy - 3 * S - g_cap_h;
}
static void modal_tab_layout(modal_t *m, int *tx, int *ty, int *tw, int *th, int *head) {
    int t = idx_of(&m->pg, 1001); ctl_t *c = t >= 0 ? &m->pg.g.c[t] : NULL;
    *tx = m->pg.ox + DX(c ? c->x : 7); *ty = m->pg.oy + DY(c ? c->y : 7); *tw = DX(c ? c->w : 100); *th = DY(c ? c->h : 100); *head = line_h() + 6 * S;
}
static void modal_open_sub(modal_t *m) {
    page_clear(&m->sub);
    if (!m->ntabs || !get_dlg(m->tabdlg[m->tab], &m->sub.g)) return;
    if (m->tabdlg[m->tab] == 153) g_cs_cat = m->tabparam[m->tab];
    dlginit(&m->sub); fill_lists(&m->sub); load_values(&m->sub); page_rules(&m->sub);
    int tx, ty, tw, th, hd; modal_tab_layout(m, &tx, &ty, &tw, &th, &hd);
    m->sub.ox = tx + 2 * S; m->sub.oy = ty + hd + 2 * S;
}
static modal_t *modal_push(void) {
    if (g_nm >= 4) return NULL;
    modal_t *m = &g_ms[g_nm]; memset(m, 0, sizeof *m); return m;
}
static void modal_place(modal_t *m) {
    g_cap_h = line_h() + 7 * S;
    int w = DX(m->pg.g.w) + 6 * S, h = DY(m->pg.g.h) + 6 * S + g_cap_h, ww, wh;
    SDL_GetRendererOutputSize(g_ren, &ww, &wh); m->save_w = 0;
    if (w > ww || h > wh) {   /* bigger than the window: grow it while the dialog is open */
        m->save_w = ww; m->save_h = wh; SDL_GetWindowPosition(g_win, &m->save_x, &m->save_y);
        int nw = (int)((w > ww ? w : ww) / g_hidpi), nh = (int)((h > wh ? h : wh) / g_hidpi), ow = (int)(ww / g_hidpi), oh = (int)(wh / g_hidpi);
        int nx = m->save_x - (nw - ow) / 2, ny = m->save_y - (nh - oh) / 2;   /* grow around the same centre, inside the screen */
        SDL_Rect b; if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(g_win), &b) == 0) {
            int bt = 0, bl = 0, bb = 0, br = 0; SDL_GetWindowBordersSize(g_win, &bt, &bl, &bb, &br); if (bt <= 0) bt = 32;
            if (nh > b.h - bt - bb) nh = b.h - bt - bb; if (nw > b.w - bl - br) nw = b.w - bl - br; nx = m->save_x - (nw - ow) / 2; ny = m->save_y - (nh - oh) / 2;
            if (nx + nw > b.x + b.w) nx = b.x + b.w - nw; if (ny + nh > b.y + b.h) ny = b.y + b.h - nh; if (nx < b.x) nx = b.x; if (ny < b.y) ny = b.y; }
        SDL_SetWindowSize(g_win, nw, nh); SDL_SetWindowPosition(g_win, nx, ny); SDL_GetRendererOutputSize(g_ren, &ww, &wh);
    }
    m->pg.ox = (ww - w) / 2 + 3 * S; m->pg.oy = (wh - h) / 2 + 3 * S + g_cap_h;
    m->scroll = 0; m->scroll_max = h > wh ? h - wh : 0; if (m->scroll_max) m->pg.oy = 3 * S + g_cap_h;
    if (w > ww) m->pg.ox = 3 * S;
    g_nm++; g_focus = -1;
    if (m->ntabs) modal_open_sub(m);
}
/* dialogs that write at once (editors of a .set file): Cancel puts the file back as it was */
static void modal_scroll(modal_t *m, int dy) {
    int n = m->scroll + dy; if (n > m->scroll_max) n = m->scroll_max; if (n < 0) n = 0;
    int d = n - m->scroll; m->scroll = n; m->pg.oy -= d; m->sub.oy -= d;
}
static void modal_backup(modal_t *m, const char *file) {
    char path[PATH_MAX]; snprintf(m->bkfile, sizeof m->bkfile, "%s", file); m->immediate = 1;
    if (lx_file_path(file, path, sizeof path)) m->bktext = lx_read_file(path, &m->bklen);
}
static void open_modal(int id) {
    modal_t *m = modal_push(); if (!m) return;
    if (!get_dlg(id, &m->pg.g)) { page_clear(&m->pg); return; }
    dlginit(&m->pg); fill_lists(&m->pg); load_values(&m->pg); page_rules(&m->pg);
    if (id == 148) {   /* the season set: three pages */
        static const char *const names[] = { "Carset", "Tyres", "Points" }; static const int pages[] = { 158, 156, 160 };
        m->ntabs = 3; for (int k = 0; k < 3; k++) { m->tabdlg[k] = pages[k]; m->tabname[k] = names[k]; }
        { char cap[256]; snprintf(cap, sizeof cap, "%s%s%s", m->pg.g.caption, m->pg.g.caption[0] ? " - " : "", season_file()); snprintf(m->pg.g.caption, sizeof m->pg.g.caption, "%s", cap); }
        modal_backup(m, season_file());
    }
    if (id == 144) {   /* car set: five pages */
        static const char *const names[] = { "Cars", "Cockpits", "Helmets", "Track", "Teams/Drivers" }; static const int pages[] = { 153, 153, 153, 155, 154 }, par[] = { 0, 1, 2, 0, 0 };
        m->ntabs = 5; for (int k = 0; k < 5; k++) { m->tabdlg[k] = pages[k]; m->tabparam[k] = par[k]; m->tabname[k] = names[k]; }
        { char cap[256]; snprintf(cap, sizeof cap, "%s - %s", m->pg.g.caption[0] ? m->pg.g.caption : "Car set", g_setfile); snprintf(m->pg.g.caption, sizeof m->pg.g.caption, "%s", cap); }
        modal_backup(m, g_setfile);
    }
    if (id == 133 && g_multi) {   /* "Multi...": same window, other title, no path line */
        snprintf(m->pg.g.caption, sizeof m->pg.g.caption, "%s", res_string(5226)); int k;
        if ((k = idx_of(&m->pg, 1641)) >= 0) m->hidden_set = 1, m->pg.hidden[k] = 1;
        if ((k = idx_of(&m->pg, 1051)) >= 0) m->pg.hidden[k] = 1;
    }
    if (id == 167) { modal_backup(m, g_fontsfile); fonts_list(&m->pg); if (strcmp(g_fontsfile, "fonts.ini")) { char cap[300]; snprintf(cap, sizeof cap, "%s - %s", m->pg.g.caption, g_fontsfile); snprintf(m->pg.g.caption, sizeof m->pg.g.caption, "%s", cap); } }
    if (id == 161) { char cap[400]; snprintf(cap, sizeof cap, "%s - %s", m->pg.g.caption, g_pnl); snprintf(m->pg.g.caption, sizeof m->pg.g.caption, "%s", cap); modal_backup(m, g_pnl); }
    if (id == 179 || id == 190) { char cap[300]; snprintf(cap, sizeof cap, "%s - %s", m->pg.g.caption, g_pnl_sec); snprintf(m->pg.g.caption, sizeof m->pg.g.caption, "%s", cap); }
    if (id == 192) { modal_backup(m, "keys.ini"); keys_list(&m->pg); snprintf(m->pg.g.caption, sizeof m->pg.g.caption, "keys.ini"); }
    if (id == 133 && !g_multi) {   /* track set: the file shown on top */
        int i = idx_of(&m->pg, 1051); if (i >= 0) snprintf(m->pg.g.c[i].text, sizeof m->pg.g.c[i].text, "gpxpdata\\%s", g_setfile);
    }
    modal_place(m);
    if (id == 161 || id == 190) pnl_sub(m);
    if (id == 143) {   /* "My IP": the addresses of this computer */
        FILE *f = popen("hostname -I 2>/dev/null", "r"); char b[512] = ""; if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; pclose(f); }
        char *tok = strtok(b, " \n"); for (int k = 0; k < 4 && tok; k++, tok = strtok(NULL, " \n")) { int i = idx_of(&m->pg, 1121 + k); if (i >= 0) snprintf(m->pg.v[i].sval, sizeof m->pg.v[i].sval, "%s", tok); }
    }
}
/* a message box in the same look (not from the language files) */
static void msgbox(const char *msg, const char *extra) {
    modal_t *m = modal_push(); if (!m) return;
    dlg_t *g = &m->pg.g; g->id = -2; snprintf(g->caption, sizeof g->caption, "GPxPatch");
    g->c = calloc(3, sizeof *g->c); g->w = 230;
    int lines = 1; for (const char *q = msg; *q; q++) if (*q == '\n') lines++;
    lines += (int)strlen(msg) / 55;
    int th = lines * 8 + 4; g->h = th + 36;
    ctl_t *c = &g->c[g->n++]; c->kind = K_STATIC; c->id = -1; c->x = 10; c->y = 10; c->w = 210; c->h = th; c->style = WS_VISIBLE; snprintf(c->text, sizeof c->text, "%s", msg);
    c = &g->c[g->n++]; c->kind = K_BUTTON; c->id = 1; c->w = 50; c->h = 14; c->y = g->h - 21; c->style = WS_VISIBLE | 1; snprintf(c->text, sizeof c->text, "OK");
    if (extra) { c->x = 230 - 10 - 50 - 6 - 100; c = &g->c[g->n++]; c->kind = K_BUTTON; c->id = 3000; c->x = 230 - 10 - 100; c->y = g->h - 21; c->w = 100; c->h = 14; c->style = WS_VISIBLE; snprintf(c->text, sizeof c->text, "%s", extra); }
    else c->x = (230 - 50) / 2;
    modal_place(m);
}
/* GPxCInfo: the page under the item fields (161) or under the image control mode (190) */
static void pnl_sub(modal_t *m) {
    page_clear(&m->sub);
    int cont = 0, dlg = 0;
    if (m->pg.g.id == 161) { cont = 1432; if (g_pnl_sec[0]) dlg = pnl_type_page(atoi(ini_get(g_pnl, g_pnl_sec, "Type", "0"))); }
    else if (m->pg.g.id == 190) { cont = 1572; int k = sel_of(&m->pg, 1553); dlg = (k == 2 || k == 5) ? 193 : (k == 0 || k == 1) ? 194 : k == 3 ? 199 : k == 4 ? 203 : 0; }
    int ci = idx_of(&m->pg, cont);
    if (!dlg || ci < 0 || !get_dlg(dlg, &m->sub.g)) { page_clear(&m->sub); return; }
    dlginit(&m->sub); fill_lists(&m->sub); load_values(&m->sub); page_rules(&m->sub);
    SDL_Rect r; crect(&m->pg, ci, &r); m->sub.ox = r.x; m->sub.oy = r.y;
}
static void pnl_open(const char *path) {
    snprintf(g_pnl, sizeof g_pnl, "pnl\\%s", path); g_pnl_sel = -1; g_pnl_sec[0] = 0;
    char fp[PATH_MAX]; if (!lx_file_path(g_pnl, fp, sizeof fp) || access(fp, R_OK)) { g_pnl[0] = 0; msgbox("O arquivo do painel não foi encontrado.\n\nThe panel file was not found.", NULL); return; }
    open_modal(161);
}
/* "new item": GPxPatch shows a menu of the 38 item types */
static void pnl_choose_type(void) {
    modal_t *m = modal_push(); if (!m) return;
    dlg_t *g = &m->pg.g; g->id = -4; snprintf(g->caption, sizeof g->caption, "%s", "GPxCInfo");
    g->c = calloc(3, sizeof *g->c); g->w = 150; g->h = 190;
    ctl_t *c = &g->c[g->n++]; c->kind = K_LIST; c->id = 3002; c->x = 7; c->y = 7; c->w = 136; c->h = 156; c->style = WS_VISIBLE;
    c = &g->c[g->n++]; c->kind = K_BUTTON; c->id = 1; c->x = 39; c->y = 169; c->w = 50; c->h = 14; c->style = WS_VISIBLE | 1; snprintf(c->text, sizeof c->text, "OK");
    c = &g->c[g->n++]; c->kind = K_BUTTON; c->id = 2; c->x = 93; c->y = 169; c->w = 50; c->h = 14; c->style = WS_VISIBLE; { int k = g_nm > 0 ? idx_of(&g_ms[g_nm - 1].pg, 2) : -1; snprintf(c->text, sizeof c->text, "%s", k >= 0 ? g_ms[g_nm - 1].pg.g.c[k].text : "Cancel"); }
    for (int k = 0; k < 38; k++) add_item(&m->pg.v[0], res_string(5080 + k));
    m->pg.v[0].ival = 0;
    modal_place(m);
}
/* a box asking for a name (STR 5060 "Enter Name") */
static void (*g_ask_done)(const char *);
static void ask_name(const char *title, void (*done)(const char *)) {
    modal_t *m = modal_push(); if (!m) return;
    dlg_t *g = &m->pg.g; g->id = -3; snprintf(g->caption, sizeof g->caption, "%s", title);
    g->c = calloc(3, sizeof *g->c); g->w = 180; g->h = 50;
    ctl_t *c = &g->c[g->n++]; c->kind = K_EDIT; c->id = 3001; c->x = 7; c->y = 8; c->w = 166; c->h = 12; c->style = WS_VISIBLE;
    c = &g->c[g->n++]; c->kind = K_BUTTON; c->id = 1; c->x = 70; c->y = 29; c->w = 50; c->h = 14; c->style = WS_VISIBLE | 1; snprintf(c->text, sizeof c->text, "OK");
    c = &g->c[g->n++]; c->kind = K_BUTTON; c->id = 2; c->x = 123; c->y = 29; c->w = 50; c->h = 14; c->style = WS_VISIBLE; snprintf(c->text, sizeof c->text, "Cancel");
    g_ask_done = done;
    modal_place(m);
    g_focus_pg = top_pg(); g_focus = 0; g_caret = 0; SDL_StartTextInput();
}
static void close_modal(int ok) {
    if (!g_in_modal) return;
    modal_t *m = &MT;
    if (ok && m->pg.g.id == -3 && g_ask_done) { char name[256]; snprintf(name, sizeof name, "%s", m->pg.v[0].sval); void (*d)(const char *) = g_ask_done; g_ask_done = NULL;
        page_clear(&m->pg); g_nm--; g_focus = -1; if (name[0]) d(name); return; }
    if (ok && m->pg.g.id == -4) { int k = m->pg.v[0].ival; page_clear(&m->pg); g_nm--; g_focus = -1;
        if (k >= 0 && g_in_modal && MT.pg.g.id == 161) { pnl_add(k); pnl_list(&MT.pg); load_values(&MT.pg); page_rules(&MT.pg); pnl_sub(&MT); } return; }
    if (ok && !m->immediate) { for (int i = 0; i < m->pg.g.n; i++) save_value(&m->pg, i); for (int i = 0; i < m->sub.g.n; i++) save_value(&m->sub, i); }
    if (!ok && m->immediate && m->bkfile[0]) {   /* Cancel: the file as it was */
        char path[PATH_MAX];
        if (lx_file_path(m->bkfile, path, sizeof path)) { if (m->bktext) { FILE *f = fopen(path, "wb"); if (f) { fwrite(m->bktext, 1, m->bklen, f); fclose(f); } } else unlink(path); }
    }
    free(m->bktext);
    if (m->pg.g.id == 133) g_multi = 0;
    if (m->pg.g.id == 161) { g_pnl[0] = 0; g_pnl_sec[0] = 0; g_pnl_sel = -1; }
    if (m->pg.g.id == 167) snprintf(g_fontsfile, sizeof g_fontsfile, "fonts.ini");
    page_clear(&m->pg); page_clear(&m->sub);
    if (m->save_w) { SDL_SetWindowSize(g_win, (int)(m->save_w / g_hidpi), (int)(m->save_h / g_hidpi)); SDL_SetWindowPosition(g_win, m->save_x, m->save_y); }
    g_nm--; g_focus = -1; g_drop = -1;
    if (g_in_modal) { if (MT.pg.g.id == 161) { pnl_fonts(&MT.pg); pnl_list(&MT.pg); } load_values(&MT.pg); page_rules(&MT.pg); if (MT.ntabs || MT.sub.g.n) { load_values(&MT.sub); page_rules(&MT.sub); } }
    else { if (g_page.g.id == 139) { int i = idx_of(&g_page, 1621), keep = i >= 0 ? g_page.v[i].ival : -1; if (i >= 0) free_items(&g_page.v[i]); fill_lists(&g_page); if (i >= 0) g_page.v[i].ival = keep; }
        load_values(&g_page); page_rules(&g_page); track_labels(&g_page); }
}
static void draw_tabbar(int tx, int ty, int tw, int th, int head, const char *const *names, int n, int selk) {
    int by = ty + head;
    fill(tx, by, tw, th - head, C_FACE);
    frame(tx, by, tw, th - head, C_HI, C_DK); frame(tx + T, by + T, tw - 2 * T, th - head - 2 * T, C_FACE, C_SH);
    int x = tx + 2 * S;
    for (int k = 0; k < n; k++) {
        int w = text_w(names[k], -1) + 12 * S; SDL_Rect r = { x, ty + 2 * S, w, head - 2 * S }; x += w;
        int sel = k == selk;
        if (sel) { r.x -= 2 * S; r.w += 4 * S; r.y -= 2 * S; r.h += 3 * S; }
        fill(r.x, r.y, r.w, r.h, C_FACE);
        vline(r.x, r.y + 2 * T, r.h - 2 * T, C_HI); fill(r.x + T, r.y + T, T, T, C_HI); hline(r.x + 2 * T, r.y, r.w - 4 * T, C_HI);
        fill(r.x + r.w - 2 * T, r.y + T, T, T, C_DK); vline(r.x + r.w - T, r.y + 2 * T, r.h - 2 * T, C_DK); vline(r.x + r.w - 2 * T, r.y + 2 * T, r.h - 2 * T, C_SH);
        if (sel) fill(r.x + T, by, r.w - 2 * T, T * 2, C_FACE);
        text(r.x + (r.w - text_w(names[k], -1)) / 2, r.y + 3 * S, names[k], -1, C_TXT);
    }
}
static int tabbar_hit(int tx, int ty, int head, const char *const *names, int n, int mx, int my) {
    int x = tx + 2 * S;
    for (int k = 0; k < n; k++) { int w = text_w(names[k], -1) + 12 * S; if (mx >= x && mx < x + w && my >= ty && my < ty + head) return k; x += w; }
    return -1;
}
static void draw_modal(void) {
    for (int k = 0; k < g_nm; k++) {
        modal_t *m = &g_ms[k]; int x, y, w, h; modal_geom(m, &x, &y, &w, &h);
        fill(x, y, w, h, C_FACE);
        frame(x, y, w, h, C_FACE, C_DK); frame(x + T, y + T, w - 2 * T, h - 2 * T, C_HI, C_SH);
        int cx = x + 3 * S, cy = y + 3 * S, cw = w - 6 * S, ch = g_cap_h - S, top = k == g_nm - 1;   /* caption bar: the Windows 2000 gradient (grey when inactive) */
        for (int q = 0; q < cw; q += T) { float t = (float)q / (float)cw;
            col_t c = top ? (col_t){ (Uint8)(10 + t * (166 - 10)), (Uint8)(36 + t * (202 - 36)), (Uint8)(106 + t * (240 - 106)) } : (col_t){ (Uint8)(128 + t * (192 - 128)), (Uint8)(128 + t * (192 - 128)), (Uint8)(128 + t * (192 - 128)) };
            fill(cx + q, cy, T, ch, c); }
        col_t tc = top ? C_HI : C_FACE;
        text(cx + 3 * S, cy + (ch - line_h()) / 2, m->pg.g.caption, -1, tc); text(cx + 3 * S + 1, cy + (ch - line_h()) / 2, m->pg.g.caption, -1, tc);
        int bs = ch - 4 * S, bx = cx + cw - bs - 2 * S, by = cy + 2 * S;   /* close button */
        fill(bx, by, bs + 2 * S, bs, C_FACE); raised(bx, by, bs + 2 * S, bs);
        static const char *const X[] = { "##    ##", " ##  ## ", "  ####  ", "   ##   ", "  ####  ", " ##  ## ", "##    ##" };
        art(bx + (bs + 2 * S - 8 * S) / 2, by + (bs - 7 * S) / 2, X, 7, C_TXT);
        if (m->ntabs) { int tx, ty, tw, th, hd; modal_tab_layout(m, &tx, &ty, &tw, &th, &hd); draw_tabbar(tx, ty, tw, th, hd, m->tabname, m->ntabs, m->tab); }
        for (int i = 0; i < m->pg.g.n; i++) draw_ctl(&m->pg, 2 + 2 * k, i);
        for (int i = 0; i < m->sub.g.n; i++) draw_ctl(&m->sub, 3 + 2 * k, i);
    }
}
static int modal_close_hit(int x, int y) {
    if (!g_in_modal) return 0;
    int mx, my, w, h; modal_geom(&MT, &mx, &my, &w, &h);
    int cx = mx + 3 * S, cy = my + 3 * S, cw = w - 6 * S, bs = g_cap_h - 5 * S;
    return x >= cx + cw - bs - 2 * S && x < cx + cw && y >= cy && y < cy + g_cap_h - S;
}
/* ------------------------------------------------------------------ */
/* the Fonts window (fonts.ini) and the Keys window (keys.ini)          */
static int ini_sections(const char *file, char out[][128], int max) {
    char fp[PATH_MAX]; size_t n; char *t = lx_file_path(file, fp, sizeof fp) ? lx_read_file(fp, &n) : NULL; int k = 0;
    for (char *q = t; q && *q && k < max; ) { char *nl = strchr(q, '\n'); while (*q == ' ') q++;
        if (*q == '[') { char *e = strchr(q, ']'); if (e && (!nl || e < nl)) { size_t l = (size_t)(e - q - 1) < 127 ? (size_t)(e - q - 1) : 127; memcpy(out[k], q + 1, l); out[k][l] = 0; k++; } }
        q = nl ? nl + 1 : NULL; }
    free(t); return k;
}
static void ini_drop_section(const char *file, const char *sec) {
    char fp[PATH_MAX]; size_t n; char *t = lx_file_path(file, fp, sizeof fp) ? lx_read_file(fp, &n) : NULL; if (!t) return;
    FILE *f = fopen(fp, "wb"); if (!f) { free(t); return; }
    int skip = 0;
    for (char *q = t; q && *q; ) { char *nl = strchr(q, '\n'); size_t l = nl ? (size_t)(nl - q + 1) : strlen(q);
        char *b = q; while (*b == ' ') b++;
        if (*b == '[') { char *e = strchr(b, ']'); skip = e && (size_t)(e - b - 1) == strlen(sec) && !strncasecmp(b + 1, sec, strlen(sec)); }
        if (!skip) fwrite(q, 1, l, f);
        q = nl ? nl + 1 : NULL; }
    fclose(f); free(t);
}
static const struct { int id; char kind; const char *key, *def; } FONTK[] = {
    { 1401, 'e', "FaceName", "" }, { 1402, 'e', "FontFile", "" }, { 1403, 'e', "Height", "12" }, { 1405, 'e', "Bold", "0" },
    { 1409, 'e', "InterLineSpacing", "0" }, { 1406, 'c', "Italic", "0" }, { 1407, 'c', "Underline", "0" }, { 1408, 'c', "StrikeOut", "0" } };
static void font_fields(page_t *p, int save) {
    for (size_t k = 0; k < sizeof FONTK / sizeof *FONTK; k++) {
        int i = idx_of(p, FONTK[k].id); if (i < 0) continue;
        p->disabled[i] = !g_font_sec[0];
        if (!g_font_sec[0]) { p->v[i].sval[0] = 0; p->v[i].ival = 0; continue; }
        if (save) { char v[32]; if (FONTK[k].kind == 'c') snprintf(v, sizeof v, "%d", p->v[i].ival); ini_put(g_fontsfile, g_font_sec, FONTK[k].key, FONTK[k].kind == 'c' ? v : p->v[i].sval); }
        else { const char *v = ini_get(g_fontsfile, g_font_sec, FONTK[k].key, FONTK[k].def); if (FONTK[k].kind == 'c') p->v[i].ival = atoi(v); else snprintf(p->v[i].sval, sizeof p->v[i].sval, "%s", v); }
    }
}
static void fonts_list(page_t *p) {
    int i = idx_of(p, 1400); if (i < 0) return;
    free_items(&p->v[i]); static char secs[64][128]; int n = ini_sections(g_fontsfile, secs, 64);
    p->v[i].ival = -1;
    for (int k = 0; k < n; k++) { add_item(&p->v[i], secs[k]); if (!strcasecmp(secs[k], g_font_sec)) p->v[i].ival = k; }
    if (p->v[i].ival < 0) { g_font_sec[0] = 0; if (n) { p->v[i].ival = 0; snprintf(g_font_sec, sizeof g_font_sec, "%s", secs[0]); } }
    font_fields(p, 0);
}
static void font_added(const char *name) {
    char n2[128]; size_t o = 0; for (const char *c = name; *c && o < sizeof n2 - 1; c++) if (*c != '[' && *c != ']' && *c != '=') n2[o++] = *c; n2[o] = 0;
    if (!n2[0]) return;
    ini_put(g_fontsfile, n2, "FaceName", "Arial"); ini_put(g_fontsfile, n2, "Height", "14");
    snprintf(g_font_sec, sizeof g_font_sec, "%s", n2);
    if (g_in_modal && MT.pg.g.id == 167) fonts_list(&MT.pg);
}
/* keys.ini [Binds]: action = key */
static void keys_list(page_t *p) {
    int i = idx_of(p, 1571); if (i < 0) return;
    free_items(&p->v[i]);
    char fp[PATH_MAX]; size_t n; char *t = lx_file_path("keys.ini", fp, sizeof fp) ? lx_read_file(fp, &n) : NULL; int in = 0;
    for (char *q = t; q && *q; ) { char *nl = strchr(q, '\n'); if (nl) *nl = 0; char *b = q; while (*b == ' ') b++; size_t l = strlen(b); while (l && (b[l - 1] == '\r' || b[l - 1] == ' ')) b[--l] = 0;
        if (*b == '[') in = !strncasecmp(b, "[Binds]", 7);
        else if (in && *b && *b != ';' && strchr(b, '=')) add_item(&p->v[i], b);
        q = nl ? nl + 1 : NULL; }
    free(t);
    if (p->v[i].ival >= p->v[i].nitems) p->v[i].ival = 0;
}
extern const char *gpx_key_name(SDL_Scancode sc);
static void key_pressed(SDL_Scancode sc) {
    page_t *p = &MT.pg; int i = idx_of(p, 1571); int row = g_key_capture; g_key_capture = -1;
    if (i < 0 || row < 0 || row >= p->v[i].nitems || sc == SDL_SCANCODE_ESCAPE) return;
    const char *n = gpx_key_name(sc); if (!n) return;
    char act[128]; snprintf(act, sizeof act, "%s", p->v[i].items[row]); char *eq = strchr(act, '='); if (!eq) return; *eq = 0;
    ini_put("keys.ini", "Binds", act, n); keys_list(p);
}

/* a file chooser of the desktop (the GPxPatch "browse" buttons) */
static int pick_file(const char *start, const char *ext, char *out, size_t outsz) {
    char cmd[PATH_MAX * 2 + 256]; out[0] = 0;
    const char *tools[] = { "zenity --file-selection --title='GPxPatch' --filename='%s/' --file-filter='*.%s *.%s' --file-filter='*' 2>/dev/null",
                            "kdialog --getopenfilename '%s' '*.%s *.%s' 2>/dev/null", NULL };
    char upper[16]; snprintf(upper, sizeof upper, "%s", ext); for (char *c = upper; *c; c++) *c = (char)toupper((unsigned char)*c);
    for (int t = 0; tools[t] && !out[0]; t++) {
        const char *bin = t == 0 ? "zenity" : "kdialog"; char chk[64]; snprintf(chk, sizeof chk, "command -v %s >/dev/null 2>&1", bin);
        if (system(chk)) continue;
        snprintf(cmd, sizeof cmd, tools[t], start, ext, upper);
        FILE *f = popen(cmd, "r"); if (!f) continue;
        if (fgets(out, (int)outsz, f)) out[strcspn(out, "\r\n")] = 0; else out[0] = 0;
        pclose(f);
        return out[0] != 0;
    }
    if (!out[0]) msgbox("Instale o \"zenity\" (ou o \"kdialog\") para escolher arquivos.\n\nInstall \"zenity\" (or \"kdialog\") to choose files.", NULL);
    return 0;
}
static void browse(page_t *p, const browse_t *b) {
    char root[PATH_MAX];
    if (b->root == 'g') snprintf(root, sizeof root, "%s", lx_dir());
    else if (b->root == 'x') { if (!lx_ci_walk(g_gpxdir, "gfx", root, sizeof root)) snprintf(root, sizeof root, "%s", g_gpxdir); }
    else if (b->root == 'n') { char d[300], rel[300]; pnl_dir(d, sizeof d); snprintf(rel, sizeof rel, "%s", d); for (char *q = rel; *q; q++) if (*q == '\\') *q = '/'; if (!lx_ci_walk(g_gpxdir, rel, root, sizeof root)) snprintf(root, sizeof root, "%s", g_gpxdir); }
    else if (b->root == 's') { if (!lx_find_ci(g_gpxdir, "snd", root, sizeof root)) snprintf(root, sizeof root, "%s", g_gpxdir); }
    else snprintf(root, sizeof root, "%s", g_gpxdir);
    char f[PATH_MAX]; if (!pick_file(root, b->ext, f, sizeof f)) return;
    size_t rl = strlen(root); const char *rel = f;
    if (!strncmp(f, root, rl) && f[rl] == '/') rel = f + rl + 1;
    else if (b->root != 'g' && b->root != 'n' && b->root != 'x' && !strncmp(f, lx_dir(), strlen(lx_dir())) && f[strlen(lx_dir())] == '/') rel = f + strlen(lx_dir()) + 1;
    else msgbox("O arquivo precisa estar dentro da pasta do jogo.\n\nThe file must be inside the game folder.", NULL);
    if (rel == f) return;
    char v[PATH_MAX]; snprintf(v, sizeof v, "%s", rel); for (char *c = v; *c; c++) if (*c == '/') *c = '\\';
    int i = idx_of(p, b->edit); if (i < 0) return;
    snprintf(p->v[i].sval, sizeof p->v[i].sval, "%s", v);
    int pg = pg_of(p);
    changed(p, pg, i); track_labels(p);
}

/* ------------------------------------------------------------------ */
/* input                                                                */
static int hit(int x, int y, int *pg, int *idx) {
    int hi = g_in_modal ? top_pg() + (MT.ntabs || MT.sub.g.n ? 1 : 0) : 1, lo = g_in_modal ? top_pg() : 0;
    for (int pp = hi; pp >= lo; pp--) {
        page_t *p = PG(pp);
        for (int i = p->g.n - 1; i >= 0; i--) {
            ctl_t *c = &p->g.c[i]; if (!visible(p, i) || !enabled(p, i)) continue;
            if (c->kind == K_STATIC || c->kind == K_TAB || (c->kind == K_BUTTON && btn_type(c) == 7)) continue;
            SDL_Rect r; crect(p, i, &r);
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) { *pg = pp; *idx = i; return 1; }
        }
    }
    return 0;
}
static void close_drop(void) { g_drop = -1; g_drop_pg = -1; }
static void mouse_down(int x, int y) {
    if (getenv("GP3_UI_DEBUG")) hlog("down %d,%d", x, y);
    if (g_drop >= 0) {
        page_t *p = PG(g_drop_pg); SDL_Rect d = drop_rect(p, g_drop);
        if (x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + d.h) {
            int ih = line_h() + 2 * S, k = (y - d.y - S) / ih + g_drop_scroll; val_t *v = &p->v[g_drop];
            if (k >= 0 && k < v->nitems) { v->ival = k; snprintf(v->sval, sizeof v->sval, "%s", v->items[k]); int pg = g_drop_pg, i = g_drop; close_drop(); changed(p, pg, i); return; }
        }
        close_drop(); return;
    }
    if (modal_close_hit(x, y)) { close_modal(0); return; }
    if (g_in_modal && MT.ntabs) {   /* the tabs of a dialog */
        int tx, ty, tw, th, hd; modal_tab_layout(&MT, &tx, &ty, &tw, &th, &hd);
        int k = tabbar_hit(tx, ty, hd, MT.tabname, MT.ntabs, x, y);
        if (k >= 0) { if (k != MT.tab) { MT.tab = k; g_focus = -1; modal_open_sub(&MT); } return; }
    }
    for (int k = 0; k < NTABS && !g_in_modal; k++) { SDL_Rect r; tab_rect(k, &r); if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) { if (k != g_tab) { g_focus = -1; open_page(k); } return; } }
    int pg, i; if (!hit(x, y, &pg, &i)) { g_focus = -1; return; }
    page_t *p = PG(pg); ctl_t *c = &p->g.c[i];
    if (c->kind == K_EDIT) { g_focus_pg = pg; g_focus = i; g_caret = (int)strlen(p->v[i].sval); SDL_StartTextInput(); return; }
    if (c->kind == K_COMBO) {
        SDL_Rect r; crect(p, i, &r);
        g_focus_pg = pg; g_focus = i; g_caret = (int)strlen(p->v[i].sval);
        if ((c->style & 3) == 2 && x < r.x + r.w - 18 * S) { SDL_StartTextInput(); return; }   /* editable part of a drop-down */
        g_drop_pg = pg; g_drop = i; g_drop_hover = p->v[i].ival; g_drop_scroll = 0;
        int vis = (drop_rect(p, i).h - 2 * S) / (line_h() + 2 * S); if (g_drop_hover >= vis) g_drop_scroll = g_drop_hover - vis + 1;
        return;
    }
    if (c->kind == K_LISTVIEW) {
        SDL_Rect r; crect(p, i, &r); int ih = line_h() + 2 * S, hh = line_h() + 4 * S; int k = (y - r.y - 2 * T - hh) / ih + p->v[i].sel_scroll;
        if (y < r.y + 2 * T + hh || k < 0 || k >= p->v[i].nitems) return;
        if (k == p->v[i].ival && p->g.id == 192) g_key_capture = k;   /* click the chosen line again, then press the new key */
        else { p->v[i].ival = k; g_key_capture = -1; }
        return;
    }
    if (c->kind == K_LIST) {
        int chk = p->g.id == 139 && c->id == 1621, ih = chk ? 15 * S : line_h() + 2 * S; SDL_Rect r; crect(p, i, &r);
        int vis = (r.h - 4 * S) / ih; val_t *lv = &p->v[i];
        if (lv->nitems > vis && x >= r.x + r.w - 2 * T - 16 * S) {   /* the scroll bar: arrows, or a page up/down */
            int sb = 16 * S; if (y < r.y + 2 * T + sb) lv->sel_scroll--; else if (y >= r.y + r.h - 2 * T - sb) lv->sel_scroll++; else lv->sel_scroll += y < r.y + r.h / 2 ? -vis : vis;
            if (lv->sel_scroll > lv->nitems - vis) lv->sel_scroll = lv->nitems - vis; if (lv->sel_scroll < 0) lv->sel_scroll = 0; return;
        }
        int k = (y - r.y - 2 * S) / ih + (lv->nitems > vis ? lv->sel_scroll : 0);
        if (k >= 0 && k < p->v[i].nitems) {
            if (p->v[i].multi) { p->v[i].ival ^= 1 << k; changed(p, pg, i); return; }
            static Uint32 last_t; static int last_k = -1, last_id; Uint32 now = SDL_GetTicks();
            int dbl = last_k == k && last_id == c->id && now - last_t < 450; last_t = now; last_k = k; last_id = c->id;
            p->v[i].ival = k;
            if (chk && dbl && x >= r.x + 18 * S) { char path[600]; snprintf(path, sizeof path, "%s", p->v[i].items[k] + 1); last_k = -1; pnl_open(path); return; }   /* double click: the panel editor */
            if (!chk) { changed(p, pg, i); if (dbl && p->g.id == -4) { last_k = -1; close_modal(1); } return; }
            if (chk && x < r.x + 18 * S) {   /* the check box: panel on/off */
                char *it = p->v[i].items[k]; it[0] = it[0] == '1' ? '0' : '1';
                char ke[16]; snprintf(ke, sizeof ke, "Enable%d", k + 1); ini_put("gpxcinfo.ini", "Panel", ke, it[0] == '1' ? "1" : "0");
            }
        }
        return;
    }
    g_press_pg = pg; g_press = i; g_hover_in = 1; g_focus = -1;
}
static void mouse_move(int x, int y) {
    if (g_drop >= 0) {
        page_t *p = PG(g_drop_pg); SDL_Rect d = drop_rect(p, g_drop);
        if (x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + d.h) { int k = (y - d.y - S) / (line_h() + 2 * S) + g_drop_scroll; if (k < p->v[g_drop].nitems) g_drop_hover = k; }
        return;
    }
    if (g_press >= 0) { page_t *p = PG(g_press_pg); SDL_Rect r; crect(p, g_press, &r); g_hover_in = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
}
static void mouse_up(int x, int y) {
    if (getenv("GP3_UI_DEBUG")) hlog("up %d,%d press %d/%d in %d", x, y, g_press_pg, g_press, g_hover_in);
    if (g_press < 0) return;
    page_t *p = PG(g_press_pg); int pg = g_press_pg, i = g_press; g_press = -1;
    if (!g_hover_in) return;
    ctl_t *c = &p->g.c[i];
    if (c->kind != K_BUTTON) return;
    int bt = btn_type(c);
    if (bt == 3 || bt == 2) { p->v[i].ival = !p->v[i].ival; changed(p, pg, i); }
    else if (bt == 6 || bt == 5) { p->v[i].ival = (p->v[i].ival + 1) % 3; changed(p, pg, i); }
    else if (bt == 9 || bt == 4) {
        for (int k = i - 1; k >= 0 && (btn_type(&p->g.c[k]) == 9 || btn_type(&p->g.c[k]) == 4) && p->g.c[k].kind == K_BUTTON; k--) { p->v[k].ival = 0; if (p->g.c[k].style & 0x00020000) break; }
        for (int k = i + 1; k < p->g.n && p->g.c[k].kind == K_BUTTON && (btn_type(&p->g.c[k]) == 9 || btn_type(&p->g.c[k]) == 4) && !(p->g.c[k].style & 0x00020000); k++) p->v[k].ival = 0;
        p->v[i].ival = 1; changed(p, pg, i);
    }
    else command(p, pg, i);
}
static void text_input(const char *t) {
    if (g_focus < 0) return;
    page_t *p = PG(g_focus_pg); ctl_t *c = &p->g.c[g_focus];
    if (!(c->kind == K_EDIT || (c->kind == K_COMBO && (c->style & 3) == 2))) return;
    char *s = p->v[g_focus].sval; size_t n = strlen(s), m = strlen(t);
    if (n + m >= sizeof p->v[g_focus].sval) return;
    if ((c->style & 0x2000) && !isdigit((unsigned char)t[0]) && t[0] != '-') return;   /* ES_NUMBER */
    memmove(s + g_caret + m, s + g_caret, n - (size_t)g_caret + 1); memcpy(s + g_caret, t, m); g_caret += (int)m;
    changed(p, g_focus_pg, g_focus);
}
static void key(SDL_Keycode k) {
    if (g_drop >= 0) {
        page_t *p = PG(g_drop_pg); val_t *v = &p->v[g_drop];
        if (k == SDLK_ESCAPE) close_drop();
        else if (k == SDLK_DOWN && g_drop_hover + 1 < v->nitems) g_drop_hover++;
        else if (k == SDLK_UP && g_drop_hover > 0) g_drop_hover--;
        else if (k == SDLK_RETURN && g_drop_hover >= 0) { v->ival = g_drop_hover; snprintf(v->sval, sizeof v->sval, "%s", v->items[g_drop_hover]); int pg = g_drop_pg, i = g_drop; close_drop(); changed(p, pg, i); }
        return;
    }
    if (g_focus < 0) { if (k == SDLK_ESCAPE) { if (g_in_modal) close_modal(0); else g_quit = 1; } if (k == SDLK_RETURN && g_in_modal) close_modal(1); return; }
    page_t *p = PG(g_focus_pg); char *s = p->v[g_focus].sval; int n = (int)strlen(s);
    ctl_t *c = &p->g.c[g_focus];
    if (c->kind == K_COMBO && (c->style & 3) == 3) {   /* drop-down list: arrows change the choice */
        val_t *v = &p->v[g_focus];
        if (k == SDLK_DOWN && v->ival + 1 < v->nitems) { v->ival++; changed(p, g_focus_pg, g_focus); }
        if (k == SDLK_UP && v->ival > 0) { v->ival--; changed(p, g_focus_pg, g_focus); }
        return;
    }
    if (k == SDLK_BACKSPACE && g_caret > 0) { int b = g_caret - 1; while (b > 0 && ((unsigned char)s[b] & 0xc0) == 0x80) b--; memmove(s + b, s + g_caret, (size_t)(n - g_caret + 1)); g_caret = b; changed(p, g_focus_pg, g_focus); }
    else if (k == SDLK_DELETE && g_caret < n) { int e = g_caret + 1; while (e < n && ((unsigned char)s[e] & 0xc0) == 0x80) e++; memmove(s + g_caret, s + e, (size_t)(n - e + 1)); changed(p, g_focus_pg, g_focus); }
    else if (k == SDLK_LEFT && g_caret > 0) { g_caret--; while (g_caret > 0 && ((unsigned char)s[g_caret] & 0xc0) == 0x80) g_caret--; }
    else if (k == SDLK_RIGHT && g_caret < n) { g_caret++; while (g_caret < n && ((unsigned char)s[g_caret] & 0xc0) == 0x80) g_caret++; }
    else if (k == SDLK_HOME) g_caret = 0;
    else if (k == SDLK_END) g_caret = n;
    else if (k == SDLK_RETURN && g_in_modal && g_focus_pg >= 2) { g_focus = -1; close_modal(1); }   /* Enter: the default button */
    else if (k == SDLK_ESCAPE && g_in_modal && g_focus_pg >= 2) { g_focus = -1; close_modal(0); }
    else if (k == SDLK_RETURN || k == SDLK_TAB || k == SDLK_ESCAPE) g_focus = -1;
}

/* ------------------------------------------------------------------ */
int gpxclassic_main(const char *game_dir, const char *version) {
    lx_setup(game_dir, version);
    if (!lx_gpxdir()[0] && lx_bundled_gpx()) lx_install_bundled_gpx();   /* first start: the original files from the package */
    if (!lx_gpxdir()[0]) { hlog("gpxclassic: GPxPatch is not installed here"); return -1; }
    snprintf(g_gpxdir, sizeof g_gpxdir, "%s", lx_gpxdir());
    scan_langs();
    if (!g_nlang) { hlog("gpxclassic: no gpxpdata/lang/GPxRes*.dll"); return -1; }
    {   /* language: the one chosen before, else the system's, else English */
        const char *want = ini_get("gp3rt.ini", "gpxpatch", "language", ""); char sys[3] = "";
        if (!*want) { const char *e = getenv("LANG"); if (e && strlen(e) >= 2) { sys[0] = (char)toupper((unsigned char)e[0]); sys[1] = (char)toupper((unsigned char)e[1]); } want = sys; }
        int k = 0; for (int i = 0; i < g_nlang; i++) if (!strcasecmp(g_langs[i].code, want)) k = i;
        if (!load_lang(k) && !load_lang(0)) return -1;
    }
    g_is2000 = !strcmp(lx_exe_version(), "2000");
    { const char *t = ini_get("gp3rt.ini", "gpxpatch", "tab", ""); for (int i = 0; i < NTABS; i++) if (!strcasecmp(TABS[i].name, t)) g_tab = i; }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { hlog("gpxclassic: SDL_Init failed: %s", SDL_GetError()); return -1; }
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#ifdef SDL_HINT_APP_NAME
    SDL_SetHint(SDL_HINT_APP_NAME, "GPxPatch");
#endif
    {   /* scale: 2x the original size; less on small screens; GP3_UI_SCALE or [gpxpatch] scale to choose */
        SDL_Rect b; if (SDL_GetDisplayUsableBounds(0, &b) == 0) {
            if (b.w < 800 || b.h < 600) S = 1;
        }
        const char *z = getenv("GP3_UI_SCALE"); if (!z || !*z) z = ini_get("gp3rt.ini", "gpxpatch", "scale", "");
        if (z && atof(z) >= 1 && atof(z) <= 4) S = (float)atof(z);
    }
    g_win = SDL_CreateWindow("GPxPatch", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 400, 300, SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
    if (!g_win) { SDL_Quit(); return -1; }
    set_window_icon_gpxp(g_win);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren || !nk_sdl_init(g_win, g_ren)) { if (g_ren) SDL_DestroyRenderer(g_ren); SDL_DestroyWindow(g_win); SDL_Quit(); return -1; }
    { int ow, oh, w, h; SDL_GetRendererOutputSize(g_ren, &ow, &oh); SDL_GetWindowSize(g_win, &w, &h); g_hidpi = w ? (float)ow / (float)w : 1; if (g_hidpi < 1) g_hidpi = 1; S = S * g_hidpi; T = S < 2 ? 1 : (int)S; }
    static const nk_rune ranges[] = { 0x0020, 0x017F, 0x0192, 0x0192, 0x02C6, 0x02DC, 0x0400, 0x045F, 0x0490, 0x0491, 0x2013, 0x2026, 0x2030, 0x2030, 0x2039, 0x203A, 0x20AC, 0x20AC, 0x2122, 0x2122, 0 };
    nk_sdl_font_stash_begin();
    g_font = nk_sdl_add_font_ranges(g_classic_font, (int)sizeof g_classic_font, 11.0f * (float)S, ranges);
    g_cjk = nk_sdl_add_font_ranges(g_classic_cjk, (int)sizeof g_classic_cjk, 11.0f * (float)S, g_cjk_ranges);
    nk_sdl_font_stash_end();
    g_ftex = nk_sdl_font_texture(); if (!g_font || !g_ftex) { hlog("gpxclassic: font failed"); return -1; }
    SDL_QueryTexture(g_ftex, NULL, NULL, &g_ftw, &g_fth);
    load_bitmaps();
    open_main();
    intro_sync();
    hlog("gpxclassic: GPxPatch screen (%s, language %s, scale %.2f)", lx_dir(), g_langs[g_lang].code, (double)S);
#ifndef _WIN32
    lx_folder_launchers();
#endif
    original_icon();   /* after the launchers: they install the icon theme */
    SDL_ShowWindow(g_win);
    { const char *o = getenv("GP3_UI_OPEN");   /* test hook: open one window straight away */
      if (o && *o) { const char *sf = getenv("GP3_UI_SET"); if (sf) snprintf(g_setfile, sizeof g_setfile, "%s", sf); g_multi = getenv("GP3_UI_MULTI") != NULL; const char *pp = getenv("GP3_UI_PNL"); if (pp) pnl_open(pp); else open_modal(atoi(o));
          const char *t = getenv("GP3_UI_TAB"); if (t && g_in_modal && MT.ntabs) { MT.tab = atoi(t); modal_open_sub(&MT); } } }
    Uint32 last_poll = 0;
    while (!g_quit) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            do {
                int mx = (int)(e.button.x * g_hidpi), my = (int)(e.button.y * g_hidpi);
                switch (e.type) {
                case SDL_QUIT: g_quit = 1; break;
                case SDL_MOUSEBUTTONDOWN: if (e.button.button == SDL_BUTTON_LEFT) mouse_down(mx, my); break;
                case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_LEFT) mouse_up(mx, my); break;
                case SDL_MOUSEMOTION: mouse_move((int)(e.motion.x * g_hidpi), (int)(e.motion.y * g_hidpi)); break;
                case SDL_MOUSEWHEEL: if (g_drop < 0) {   /* scroll the list under the mouse */
                        int wx, wy, hp, hi2; SDL_GetMouseState(&wx, &wy); wx = (int)(wx * g_hidpi); wy = (int)(wy * g_hidpi);
                        if (hit(wx, wy, &hp, &hi2) && PG(hp)->g.c[hi2].kind == K_LIST) { val_t *v = &PG(hp)->v[hi2]; v->sel_scroll -= e.wheel.y * 3; if (v->sel_scroll < 0) v->sel_scroll = 0; break; }
                        if (g_in_modal && MT.scroll_max) { modal_scroll(&MT, -e.wheel.y * (int)(40 * S)); break; }
                    }
                    if (g_drop < 0 && g_in_modal) {   /* scroll a list view */
                        page_t *p = &MT.pg; for (int i = 0; i < p->g.n; i++) if (p->g.c[i].kind == K_LISTVIEW) { val_t *v = &p->v[i]; v->sel_scroll -= e.wheel.y * 3; if (v->sel_scroll < 0) v->sel_scroll = 0; if (v->sel_scroll > v->nitems - 1) v->sel_scroll = v->nitems > 0 ? v->nitems - 1 : 0;
                            }
                    }
                    if (g_drop >= 0) { page_t *p = PG(g_drop_pg); int vis = (drop_rect(p, g_drop).h - 2 * S) / (line_h() + 2 * S);
                        g_drop_scroll -= e.wheel.y; if (g_drop_scroll > p->v[g_drop].nitems - vis) g_drop_scroll = p->v[g_drop].nitems - vis; if (g_drop_scroll < 0) g_drop_scroll = 0; } break;
                case SDL_TEXTINPUT: text_input(e.text.text); break;
                case SDL_KEYDOWN: if (g_key_capture >= 0 && g_in_modal && MT.pg.g.id == 192) key_pressed(e.key.keysym.scancode); else key(e.key.keysym.sym); break;
                default: break;
                }
            } while (SDL_PollEvent(&e));
        }
        Uint32 now = SDL_GetTicks();
        if (now - last_poll > 300) {
            last_poll = now;
            if (g_running && g_hide_at && now >= g_hide_at) { g_hide_at = 0; if (lx_game_running()) { SDL_HideWindow(g_win); g_hidden = 1; } }
            if (g_running && !lx_game_running()) {   /* the game closed: the screen comes back */
                g_running = 0; g_hide_at = 0;
                if (g_hidden) { SDL_ShowWindow(g_win); g_hidden = 0; }
                SDL_RestoreWindow(g_win); SDL_RaiseWindow(g_win);
            }
        }
        draw_all();
        SDL_RenderPresent(g_ren);
    }
    page_clear(&g_page); page_clear(&g_main); pe_free(&g_res);
    nk_sdl_shutdown(); SDL_DestroyRenderer(g_ren); SDL_DestroyWindow(g_win); SDL_Quit();
    hlog("gpxclassic: closed");
    return 0;
}
