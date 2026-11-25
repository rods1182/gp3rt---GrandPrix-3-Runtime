/* GP3RT for Linux - GPxPatch configuration screen, native (SDL2 + Nuklear).
 *
 * The same tabs and options as the browser screen (host/launcher_ui.html) and as the
 * Windows GPxPatch: it reads and writes gpxpdata\*.ini, keys.ini, fonts.ini, the track,
 * season and car sets, and gp3rt.ini, and starts the game with or without GPxPatch.
 * File work is shared with the browser screen (host/launcher.c, launcher.h).
 *
 * "gp3 --gpxpatch" opens this window; "gp3 --gpxpatch-web" the browser page. When this
 * one cannot run (no display, SDL older than 2.0.18) the browser page is used instead.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <limits.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "win/win_compat.h"
#define GP3_OS_NAME "Windows"
#else
#define GP3_OS_NAME "Linux"
#endif
#include "nk_conf.h"
#include "launcher.h"
#include "ui_i18n.h"        /* generated from launcher_ui.html: g_i18n[][2] = { pt, en } */
#include "ui_fonts.h"       /* generated: Inter subset (OFL) */

void hlog(const char *fmt, ...);
extern void set_window_icon_gpxp(SDL_Window *w);

/* ------------------------------------------------------------------ */
/* look                                                                 */
static float g_sc = 1;                    /* drawable pixels per window point (HiDPI) */
#define U(x) ((float)(x) * g_sc)
static struct nk_context *ctx;
static SDL_Window *g_win; static SDL_Renderer *g_ren;
enum { F_TEXT, F_SMALL, F_BOLD, F_TITLE, F_H3, F_N };
static struct nk_font *g_font[F_N];
static const struct nk_user_font *FONT(int i) { return &g_font[i]->handle; }

#undef RGB
#define RGB(r, g, b) nk_rgb(r, g, b)
static struct nk_color C_BG, C_PANEL, C_PANEL2, C_LINE, C_TEXT, C_MUTED, C_ACCENT, C_ACCENT2, C_OK, C_WARN, C_FIELD;
static void theme(void) {
    C_BG = RGB(0x15, 0x17, 0x1a); C_PANEL = RGB(0x1d, 0x20, 0x24); C_PANEL2 = RGB(0x24, 0x28, 0x2d); C_LINE = RGB(0x33, 0x38, 0x3f);
    C_TEXT = RGB(0xe7, 0xe9, 0xec); C_MUTED = RGB(0x9a, 0xa1, 0xab); C_ACCENT = RGB(0xe8, 0x43, 0x2f); C_ACCENT2 = RGB(0xff, 0x6a, 0x4d);
    C_OK = RGB(0x3f, 0xb9, 0x50); C_WARN = RGB(0xe3, 0xb3, 0x41); C_FIELD = RGB(0x11, 0x13, 0x16);
    struct nk_color t[NK_COLOR_COUNT];
    t[NK_COLOR_TEXT] = C_TEXT; t[NK_COLOR_WINDOW] = C_BG; t[NK_COLOR_HEADER] = C_PANEL; t[NK_COLOR_BORDER] = C_LINE;
    t[NK_COLOR_BUTTON] = C_PANEL2; t[NK_COLOR_BUTTON_HOVER] = RGB(0x2e, 0x33, 0x39); t[NK_COLOR_BUTTON_ACTIVE] = RGB(0x38, 0x3e, 0x45);
    t[NK_COLOR_TOGGLE] = C_FIELD; t[NK_COLOR_TOGGLE_HOVER] = C_PANEL2; t[NK_COLOR_TOGGLE_CURSOR] = C_ACCENT;
    t[NK_COLOR_SELECT] = C_PANEL2; t[NK_COLOR_SELECT_ACTIVE] = C_ACCENT;
    t[NK_COLOR_SLIDER] = C_FIELD; t[NK_COLOR_SLIDER_CURSOR] = C_ACCENT; t[NK_COLOR_SLIDER_CURSOR_HOVER] = C_ACCENT2; t[NK_COLOR_SLIDER_CURSOR_ACTIVE] = C_ACCENT2;
    t[NK_COLOR_PROPERTY] = C_FIELD; t[NK_COLOR_EDIT] = C_FIELD; t[NK_COLOR_EDIT_CURSOR] = C_TEXT;
    t[NK_COLOR_COMBO] = C_FIELD; t[NK_COLOR_CHART] = C_PANEL2; t[NK_COLOR_CHART_COLOR] = C_ACCENT; t[NK_COLOR_CHART_COLOR_HIGHLIGHT] = C_ACCENT2;
    t[NK_COLOR_SCROLLBAR] = C_BG; t[NK_COLOR_SCROLLBAR_CURSOR] = RGB(0x44, 0x4a, 0x52); t[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = RGB(0x55, 0x5c, 0x66);
    t[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = RGB(0x66, 0x6e, 0x78); t[NK_COLOR_TAB_HEADER] = C_PANEL;
    t[NK_COLOR_KNOB] = C_FIELD; t[NK_COLOR_KNOB_CURSOR] = C_ACCENT; t[NK_COLOR_KNOB_CURSOR_HOVER] = C_ACCENT2; t[NK_COLOR_KNOB_CURSOR_ACTIVE] = C_ACCENT2;
    nk_style_from_table(ctx, t);
    struct nk_style *s = &ctx->style;
    s->window.padding = nk_vec2(0, 0); s->window.spacing = nk_vec2(0, 0); s->window.border = 0;
    s->window.group_padding = nk_vec2(0, 0); s->window.scrollbar_size = nk_vec2(U(10), U(10));
    s->button.rounding = U(7); s->button.border = U(1); s->button.border_color = C_LINE; s->button.padding = nk_vec2(U(10), U(4));
    s->edit.rounding = U(6); s->edit.border = U(1); s->edit.border_color = C_LINE; s->edit.padding = nk_vec2(U(6), U(4));
    s->combo.rounding = U(6); s->combo.border = U(1); s->combo.border_color = C_LINE; s->combo.content_padding = nk_vec2(U(8), U(4));
    s->combo.button.normal = nk_style_item_color(C_FIELD); s->combo.button.hover = nk_style_item_color(C_FIELD); s->combo.button.active = nk_style_item_color(C_FIELD);
    s->combo.button.text_normal = C_MUTED; s->combo.button.text_hover = C_TEXT; s->combo.button.text_active = C_TEXT;
    s->combo.button_padding = nk_vec2(U(9), U(10)); s->combo.sym_normal = s->combo.sym_hover = s->combo.sym_active = NK_SYMBOL_TRIANGLE_DOWN;
    s->combo.hover = nk_style_item_color(C_FIELD); s->combo.active = nk_style_item_color(C_FIELD); s->combo.normal = nk_style_item_color(C_FIELD);
    s->combo.label_normal = C_TEXT; s->combo.label_hover = C_TEXT; s->combo.label_active = C_TEXT;
    s->property.normal = nk_style_item_color(C_FIELD); s->property.hover = nk_style_item_color(C_FIELD); s->property.active = nk_style_item_color(C_FIELD);
    s->property.label_normal = C_TEXT; s->property.label_hover = C_TEXT; s->property.label_active = C_TEXT;
    s->property.sym_left = NK_SYMBOL_TRIANGLE_LEFT; s->property.sym_right = NK_SYMBOL_TRIANGLE_RIGHT;
    s->property.dec_button.normal = s->property.inc_button.normal = nk_style_item_color(C_FIELD);
    s->property.dec_button.hover = s->property.inc_button.hover = nk_style_item_color(C_FIELD);
    s->property.dec_button.text_normal = s->property.inc_button.text_normal = C_MUTED;
    s->property.dec_button.text_hover = s->property.inc_button.text_hover = C_TEXT;
    s->property.edit.normal = s->property.edit.hover = s->property.edit.active = nk_style_item_color(C_FIELD);
    s->edit.normal = s->edit.hover = nk_style_item_color(C_FIELD); s->edit.active = nk_style_item_color(nk_rgb(0x0c, 0x0e, 0x10));
    s->edit.border_color = C_LINE; s->edit.cursor_normal = C_TEXT; s->edit.selected_normal = C_ACCENT; s->edit.selected_hover = C_ACCENT;
    s->button.text_normal = C_TEXT; s->button.text_hover = C_TEXT; s->button.text_active = C_TEXT;
    s->selectable.normal = nk_style_item_color(nk_rgba(0, 0, 0, 0)); s->selectable.hover = nk_style_item_color(C_PANEL2); s->selectable.pressed = nk_style_item_color(C_PANEL2);
    s->selectable.text_normal = C_TEXT; s->selectable.text_hover = C_TEXT; s->selectable.padding = nk_vec2(U(8), U(4));
    s->property.rounding = U(6); s->property.border = U(1); s->property.border_color = C_LINE;
    s->contextual_button.padding = nk_vec2(U(8), U(4));
    s->contextual_button.hover = nk_style_item_color(C_PANEL2);
    s->window.popup_border = U(1); s->window.popup_border_color = C_LINE; s->window.popup_padding = nk_vec2(U(14), U(12));
    s->window.combo_border = U(1); s->window.combo_border_color = C_LINE; s->window.combo_padding = nk_vec2(U(2), U(2));
    s->scrollv.rounding_cursor = U(5); s->scrollh.rounding_cursor = U(5);
}

/* ------------------------------------------------------------------ */
/* language                                                             */
static int g_en;
static const char *L(const char *pt) {
    if (!g_en || !pt) return pt;
    for (size_t i = 0; i < sizeof g_i18n / sizeof g_i18n[0]; i++) if (!strcmp(g_i18n[i][0], pt)) return g_i18n[i][1];
    return pt;
}
/* L() with {0} {1} {2} arguments */
static const char *Lf(const char *pt, const char *a0, const char *a1, const char *a2) {
    static char out[4][1024]; static int n; char *o = out[n++ & 3]; size_t w = 0;
    const char *a[3] = { a0, a1, a2 };
    for (const char *s = L(pt); *s && w < 1020; s++) {
        if (s[0] == '{' && s[1] >= '0' && s[1] <= '2' && s[2] == '}') { const char *v = a[s[1] - '0'] ? a[s[1] - '0'] : ""; while (*v && w < 1020) o[w++] = *v++; s += 2; }
        else o[w++] = *s;
    }
    o[w] = 0; return o;
}
static char g_status[512]; static Uint32 g_status_t;
static void status(const char *s) { snprintf(g_status, sizeof g_status, "%s", s); g_status_t = SDL_GetTicks(); }
static Uint32 g_saved_t; static int g_saved_fail;

/* ------------------------------------------------------------------ */
/* .ini model (values in UTF-8; files are Windows-1252 unless saved as UTF-8)  */
typedef struct ikey { char *k, *v; struct ikey *next; } ikey;
typedef struct isec { char *name; ikey *keys, *last; struct isec *next; } isec;
typedef struct ifile { char name[256]; int utf8, exists; isec *secs, *last; struct ifile *next; } ifile;
static ifile *g_files;

static char *latin1_to_utf8(const char *s, size_t n) {
    char *o = malloc(n * 2 + 1); size_t w = 0;
    for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)s[i]; if (c < 0x80) o[w++] = (char)c; else { o[w++] = (char)(0xc0 | (c >> 6)); o[w++] = (char)(0x80 | (c & 0x3f)); } }
    o[w] = 0; return o;
}
static isec *sec_find(ifile *f, const char *s, int create) {
    for (isec *x = f->secs; x; x = x->next) if (!strcasecmp(x->name, s)) return x;
    if (!create) return NULL;
    isec *x = calloc(1, sizeof *x); x->name = strdup(s);
    if (f->last) f->last->next = x; else f->secs = x; f->last = x; return x;
}
static ikey *key_find(isec *x, const char *k) { for (ikey *e = x->keys; e; e = e->next) if (!strcasecmp(e->k, k)) return e; return NULL; }
static void file_free(ifile *f) {
    for (isec *x = f->secs; x;) { isec *n = x->next; for (ikey *e = x->keys; e;) { ikey *m = e->next; free(e->k); free(e->v); free(e); e = m; } free(x->name); free(x); x = n; }
    f->secs = f->last = NULL;
}
static void file_parse(ifile *f, const char *text) {
    isec *cur = NULL;
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[2048]; if (n >= sizeof line) n = sizeof line - 1; memcpy(line, p, n); line[n] = 0;
        p += n + (e ? 1 : 0);
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        char *t = s + strlen(s); while (t > s && (t[-1] == '\r' || t[-1] == ' ' || t[-1] == '\t')) *--t = 0;
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *b = strchr(s, ']'), *eq = strchr(s, '=');
            if (eq && (!b || eq < b)) continue;           /* "[Jam01=...]": a disabled entry */
            if (b) { *b = 0; cur = sec_find(f, s + 1, 1); }
            continue;
        }
        char *eq = strchr(s, '='); if (!eq || !cur) continue;
        *eq = 0; char *k = s, *v = eq + 1; char *ke = eq; while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        while (*v == ' ' || *v == '\t') v++;
        ikey *x = key_find(cur, k);
        if (x) { free(x->v); x->v = strdup(v); }
        else { x = calloc(1, sizeof *x); x->k = strdup(k); x->v = strdup(v); if (cur->last) cur->last->next = x; else cur->keys = x; cur->last = x; }
    }
}
static ifile *ini(const char *name) {   /* load once (and on reload) */
    for (ifile *f = g_files; f; f = f->next) if (!strcasecmp(f->name, name)) return f;
    ifile *f = calloc(1, sizeof *f); snprintf(f->name, sizeof f->name, "%s", name);
    f->next = g_files; g_files = f;
    char p[PATH_MAX]; size_t n = 0; char *t = lx_file_path(name, p, sizeof p) ? lx_read_file(p, &n) : NULL;
    if (t) {
        f->exists = 1; f->utf8 = lx_is_utf8(t, n);
        if (f->utf8) file_parse(f, t); else { char *u = latin1_to_utf8(t, n); file_parse(f, u); free(u); }
        free(t);
    }
    return f;
}
static void ini_forget(const char *name) {
    for (ifile **pp = &g_files; *pp; pp = &(*pp)->next) if (!strcasecmp((*pp)->name, name)) { ifile *f = *pp; *pp = f->next; file_free(f); free(f); return; }
}
static void ini_forget_all(void) { while (g_files) { ifile *f = g_files; g_files = f->next; file_free(f); free(f); } }
static const char *get(const char *f, const char *s, const char *k, const char *d) {
    isec *x = sec_find(ini(f), s, 0); ikey *e = x ? key_find(x, k) : NULL;
    return e ? e->v : (d ? d : "");
}
static int has(const char *f, const char *s, const char *k) { isec *x = sec_find(ini(f), s, 0); return x && key_find(x, k); }
static int geti(const char *f, const char *s, const char *k, int d) { const char *v = get(f, s, k, NULL); return *v ? atoi(v) : d; }
static int sec_exists(const char *f, const char *s) { return sec_find(ini(f), s, 0) != NULL; }

/* writes are queued and written together (a dragged number makes many changes) */
typedef struct pend { char f[256], s[128], k[128]; char *v; int del; struct pend *next; } pend;
static pend *g_pend; static Uint32 g_pend_t;
static void put(const char *f, const char *s, const char *k, const char *v) {   /* v NULL: delete the key */
    ifile *F = ini(f); isec *x = sec_find(F, s, v != NULL);
    if (x) {
        ikey *e = key_find(x, k);
        if (v) { if (e) { if (!strcmp(e->v, v)) return; free(e->v); e->v = strdup(v); }
                 else { e = calloc(1, sizeof *e); e->k = strdup(k); e->v = strdup(v); if (x->last) x->last->next = e; else x->keys = e; x->last = e; } }
        else if (e) { ikey **pp = &x->keys; ikey *prev = NULL; while (*pp != e) { prev = *pp; pp = &(*pp)->next; } *pp = e->next; if (x->last == e) x->last = prev; free(e->k); free(e->v); free(e); }
        else return;
    } else if (!v) return;
    for (pend *q = g_pend; q; q = q->next)
        if (!strcasecmp(q->f, f) && !strcasecmp(q->s, s) && !strcasecmp(q->k, k)) { free(q->v); q->v = v ? strdup(v) : NULL; q->del = !v; g_pend_t = SDL_GetTicks(); return; }
    pend *q = calloc(1, sizeof *q);
    snprintf(q->f, sizeof q->f, "%s", f); snprintf(q->s, sizeof q->s, "%s", s); snprintf(q->k, sizeof q->k, "%s", k);
    q->v = v ? strdup(v) : NULL; q->del = !v;
    pend **pp = &g_pend; while (*pp) pp = &(*pp)->next; *pp = q;
    g_pend_t = SDL_GetTicks();
}
static void puti(const char *f, const char *s, const char *k, int v) { char b[32]; snprintf(b, sizeof b, "%d", v); put(f, s, k, b); }
static int flush(void) {
    int bad = 0, n = 0;
    while (g_pend) {
        pend *q = g_pend; g_pend = q->next;
        char p[PATH_MAX];
        ifile *F = ini(q->f);
        if (lx_file_path(q->f, p, sizeof p)) {
            char s[256], k[256], *v = q->v ? strdup(q->v) : NULL;
            snprintf(s, sizeof s, "%s", q->s); snprintf(k, sizeof k, "%s", q->k);
            if (!F->utf8) { lx_utf8_to_latin1(s); lx_utf8_to_latin1(k); if (v) lx_utf8_to_latin1(v); }
            if (lx_ini_set(p, s, k, v)) { n++; F->exists = 1; } else bad++;
            free(v);
        } else bad++;
        free(q->v); free(q);
    }
    if (n || bad) { g_saved_t = SDL_GetTicks(); g_saved_fail = bad; }
    return !bad;
}

/* ------------------------------------------------------------------ */
/* lists of files (like /api/state of the browser screen)               */
typedef struct { char **v; int n, cap; } strv;
static void sv_add(strv *a, const char *s) { if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->v = realloc(a->v, sizeof(char *) * (size_t)a->cap); } a->v[a->n++] = strdup(s); }
static void sv_clear(strv *a) { for (int i = 0; i < a->n; i++) free(a->v[i]); a->n = 0; }
static int sv_has(const strv *a, const char *s) { for (int i = 0; i < a->n; i++) if (!strcasecmp(a->v[i], s)) return 1; return 0; }
static int natcmp(const char *a, const char *b) {   /* car1 < car1a < car2 < car10 */
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);
            if (x != y) return x < y ? -1 : 1; continue;
        }
        int c = tolower((unsigned char)*a) - tolower((unsigned char)*b); if (c) return c; a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
static int natcmp_p(const void *a, const void *b) { return natcmp(*(char *const *)a, *(char *const *)b); }
static void sv_sort(strv *a) { if (a->n > 1) qsort(a->v, (size_t)a->n, sizeof(char *), natcmp_p); }

static struct {
    int gpx, gpxsup, is2000, shortcuts, ddraw;
    strv sets, seasons, carsets, dats, skies, tarmacs, wavs, panels, ttfs, circuit_inis;
} S;
static int is_dir(const char *p) { DIR *d = opendir(p); if (d) { closedir(d); return 1; } return 0; }
static void list_rec(strv *out, const char *abs, const char *shown, const char *ext, int depth) {
    DIR *d = opendir(abs); if (!d) return; struct dirent *e;
    while ((e = readdir(d)) && out->n < 2000) {
        if (e->d_name[0] == '.') continue;
        char full[PATH_MAX], sh[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", abs, e->d_name); snprintf(sh, sizeof sh, "%s%s%s", shown, shown[0] ? "\\" : "", e->d_name);
        if (is_dir(full)) { if (depth > 0 && strcasecmp(e->d_name, "gpxpdata") && strcasecmp(e->d_name, "screens") && strcasecmp(e->d_name, "gpsaves")) list_rec(out, full, sh, ext, depth - 1); }
        else { const char *dot = strrchr(e->d_name, '.'); if (dot && !strcasecmp(dot, ext)) sv_add(out, sh); }
    }
    closedir(d);
}
static char *ci_strstr(const char *h, const char *n) {
    size_t l = strlen(n); for (; *h; h++) if (!strncasecmp(h, n, l)) return (char *)h; return NULL;
}
static void scan(void) {
    const char *dir = lx_dir(), *gd = lx_gpxdir(); char p[PATH_MAX];
    S.gpx = gd[0] != 0; S.gpxsup = lx_find_ci(dir, "GPxPSup.dll", p, sizeof p); S.is2000 = !strcmp(lx_exe_version(), "2000"); S.ddraw = lx_find_ci(dir, "ddraw.dll", p, sizeof p);
    S.shortcuts = lx_shortcuts_exist();
    strv *all[] = { &S.sets, &S.seasons, &S.carsets, &S.dats, &S.skies, &S.tarmacs, &S.wavs, &S.panels, &S.ttfs, &S.circuit_inis };
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) sv_clear(all[i]);
    if (gd[0]) {
        DIR *d = opendir(gd); struct dirent *e;
        while (d && (e = readdir(d))) { const char *dot = strrchr(e->d_name, '.'); if (!dot || strcasecmp(dot, ".set")) continue;
            char f[PATH_MAX * 2]; snprintf(f, sizeof f, "%s/%s", gd, e->d_name); char k = lx_set_kind(f);
            sv_add(k == 's' ? &S.seasons : k == 'c' ? &S.carsets : &S.sets, e->d_name); }
        if (d) closedir(d);
        char sub[PATH_MAX];
        if (lx_find_ci(gd, "snd", sub, sizeof sub)) list_rec(&S.wavs, sub, "", ".wav", 1);
        if (lx_find_ci(gd, "pnl", sub, sizeof sub)) list_rec(&S.panels, sub, "", ".ini", 2);
        if (lx_ci_walk(gd, "gfx/fonts", sub, sizeof sub)) list_rec(&S.ttfs, sub, "fonts", ".ttf", 1);
    }
    list_rec(&S.dats, dir, "", ".dat", 4);
    if (lx_find_ci(dir, "circuits", p, sizeof p)) { DIR *d = opendir(p); struct dirent *e;
        while (d && (e = readdir(d))) { const char *dot = strrchr(e->d_name, '.'); if (dot && !strcasecmp(dot, ".ini")) sv_add(&S.circuit_inis, e->d_name); }
        if (d) closedir(d); }
    if (lx_ci_walk(dir, "gp3jamsh/techjam", p, sizeof p)) { DIR *d = opendir(p); struct dirent *e;
        while (d && (e = readdir(d))) { const char *n = e->d_name; size_t l = strlen(n);
            if (l < 8 || strcasecmp(n + l - 4, ".bmp")) continue;
            if (!strncasecmp(n, "sky", 3) && l >= 9 && isdigit((unsigned char)n[l - 5]) && isdigit((unsigned char)n[l - 6])) {
                char pre[64]; size_t pl = l - 6 < sizeof pre - 1 ? l - 6 : sizeof pre - 1; memcpy(pre, n, pl); pre[pl] = 0;
                for (char *c = pre; *c; c++) *c = (char)tolower((unsigned char)*c);
                if (!sv_has(&S.skies, pre)) sv_add(&S.skies, pre);
            } else if (ci_strstr(n, "tarmac")) sv_add(&S.tarmacs, n);
        }
        if (d) closedir(d); }
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) sv_sort(all[i]);
}
static int file_exists_win(const char *v) {   /* a GP3 path (UTF-8): exists? */
    char hp[PATH_MAX], l1[PATH_MAX];
    if (lx_game_file(v, hp, sizeof hp)) return 1;
    snprintf(l1, sizeof l1, "%s", v); lx_utf8_to_latin1(l1);
    return lx_game_file(l1, hp, sizeof hp);
}

/* ------------------------------------------------------------------ */
/* drawing helpers                                                      */
static float text_w(int font, const char *s, int n) { const struct nk_user_font *f = FONT(font); return f->width(f->userdata, f->height, s, n < 0 ? (int)strlen(s) : n); }
static void draw_text(struct nk_command_buffer *c, int font, float x, float y, float w, const char *s, struct nk_color col) {
    const struct nk_user_font *f = FONT(font);
    nk_draw_text(c, nk_rect(x, y, w, f->height + U(2)), s, (int)strlen(s), f, nk_rgba(0, 0, 0, 0), col);
}
/* word wrap: returns the number of lines; draws them when c != NULL */
static int wrap_text(struct nk_command_buffer *c, int font, float x, float y, float w, const char *s, struct nk_color col, float lh) {
    int lines = 0; const char *p = s;
    while (*p) {
        const char *best = NULL, *q = p;
        for (;;) {
            const char *sp = strchr(q, ' '); const char *end = sp ? sp : q + strlen(q);
            if (text_w(font, p, (int)(end - p)) > w && best) break;
            best = end; if (!sp) break; q = sp + 1;
        }
        if (c) { const struct nk_user_font *f = FONT(font); nk_draw_text(c, nk_rect(x, y + lines * lh, w + U(40), lh), p, (int)(best - p), f, nk_rgba(0, 0, 0, 0), col); }
        lines++; p = best; while (*p == ' ') p++;
    }
    return lines ? lines : 1;
}

/* ------------------------------------------------------------------ */
/* layout: groups (cards) and rows (label + hint on the left, control on the right)  */
static float g_cw;                 /* width of the content column (previous frame) */
static int g_dry;                  /* building tabs off screen: only write missing defaults */
static struct { char id[96]; float y0; float h; } g_grp[64]; static int g_ngrp, g_curgrp = -1;
static float g_grp_h[256]; static char g_grp_id[256][96]; static int g_ngrp_h;
static float *grp_height(const char *id) {
    for (int i = 0; i < g_ngrp_h; i++) if (!strcmp(g_grp_id[i], id)) return &g_grp_h[i];
    if (g_ngrp_h < 256) { snprintf(g_grp_id[g_ngrp_h], 96, "%s", id); g_grp_h[g_ngrp_h] = 0; return &g_grp_h[g_ngrp_h++]; }
    return &g_grp_h[0];
}
/* begin a free-layout row of height h with up to n widgets; returns its absolute rectangle
   (nk_layout_space_bounds gives the panel origin, not the row's) */
static struct nk_rect space_row(float h, float w, int n) {
    nk_layout_space_begin(ctx, NK_STATIC, h, n + 1);
    nk_layout_space_push(ctx, nk_rect(0, 0, w, h));
    struct nk_rect r; nk_widget(&r, ctx);
    return r;
}
static void space(float h) { nk_layout_row_dynamic(ctx, h, 1); nk_spacing(ctx, 1); }
static int g_rowfirst;
static void group_begin(const char *title, const char *note) {
    if (g_dry) return;
    char id[96]; snprintf(id, sizeof id, "%s", title);
    nk_layout_row_dynamic(ctx, U(1), 1);
    struct nk_rect b = nk_layout_widget_bounds(ctx);
    float *h = grp_height(id);
    struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    float sy = b.y - (float)*ctx->current->layout->offset_y;   /* (nk_layout_widget_bounds ignores the scroll position) */
    if (*h > 0) { nk_fill_rect(cv, nk_rect(b.x, sy, b.w, *h), U(10), C_PANEL); nk_stroke_rect(cv, nk_rect(b.x, sy, b.w, *h), U(10), U(1), C_LINE); }
    nk_spacing(ctx, 1);
    g_curgrp = 0; snprintf(g_grp[0].id, sizeof g_grp[0].id, "%s", id); g_grp[0].y0 = b.y;
    nk_layout_row_dynamic(ctx, U(28), 1);
    struct nk_rect t; if (nk_widget(&t, ctx)) {
        char up[256]; snprintf(up, sizeof up, "%s", L(title));
        for (unsigned char *c = (unsigned char *)up; *c; c++) {   /* ASCII and Latin-1 letters (UTF-8) */
            if (*c < 128) *c = (unsigned char)toupper(*c);
            else if (*c == 0xc3 && c[1] >= 0xa0 && c[1] <= 0xbe && c[1] != 0xb7) { c[1] -= 0x20; c++; }
        }
        draw_text(cv, F_H3, t.x + U(16), t.y + U(10), t.w - U(32), up, C_MUTED);
    }
    if (note && *note) {
        float lw = g_cw - U(32); int n = wrap_text(NULL, F_SMALL, 0, 0, lw, L(note), C_MUTED, U(17));
        nk_layout_row_dynamic(ctx, n * U(17) + U(4), 1);
        struct nk_rect r; if (nk_widget(&r, ctx)) wrap_text(cv, F_SMALL, r.x + U(16), r.y + U(2), lw, L(note), C_MUTED, U(17));
    }
    g_rowfirst = 1;
}
static void group_end(void) {
    if (g_dry) return;
    nk_layout_row_dynamic(ctx, U(6), 1);
    struct nk_rect b = nk_layout_widget_bounds(ctx); nk_spacing(ctx, 1);
    *grp_height(g_grp[0].id) = b.y + b.h - g_grp[0].y0;
    if (getenv("GP3_UI_DEBUG")) hlog("group %s: y0 %.0f end %.0f", g_grp[0].id, g_grp[0].y0, b.y + b.h);
    space(U(12));
    g_curgrp = -1;
}
/* a row: returns the rectangle for the control(s) on the right (cw wide) */
static struct nk_rect g_row; static int g_row_ok;
static int row_begin(const char *label, const char *hint, float cw) {
    float pad = U(16), gap = U(14);
    float lw = g_cw - 2 * pad - cw - gap; if (lw < U(120)) lw = U(120);
    int hl = hint && *hint ? wrap_text(NULL, F_SMALL, 0, 0, lw, L(hint), C_MUTED, U(16)) : 0;
    int ll = label && *label ? wrap_text(NULL, F_TEXT, 0, 0, lw, L(label), C_TEXT, U(19)) : 1;
    float h = ll * U(19) + hl * U(16) + U(16); if (h < U(44)) h = U(44);
    struct nk_rect area = space_row(h, g_cw, 8);
    struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    if (!g_rowfirst) nk_stroke_line(cv, area.x + pad, area.y, area.x + area.w - pad, area.y, U(1), nk_rgb(0x2a, 0x2e, 0x34));
    g_rowfirst = 0;
    float ty = area.y + (h - (ll * U(19) + hl * U(16))) / 2;
    if (label && *label) wrap_text(cv, F_TEXT, area.x + pad, ty, lw, L(label), C_TEXT, U(19));
    if (hl) wrap_text(cv, F_SMALL, area.x + pad, ty + ll * U(19), lw, L(hint), C_MUTED, U(16));
    g_row = nk_rect(g_cw - pad - cw, 0, cw, h);   /* relative to the row */
    g_row_ok = 1;
    return 1;
}
static void row_end(void) { nk_layout_space_end(ctx); }
/* place a control of width w, height h at x (relative to the control area) */
static void row_push(float x, float w, float h) { nk_layout_space_push(ctx, nk_rect(g_row.x + x, (g_row.h - h) / 2, w, h)); }

/* ------------------------------------------------------------------ */
/* controls                                                             */
static int ui_switch(int on) {
    struct nk_rect b; enum nk_widget_layout_states st = nk_widget(&b, ctx);
    if (!st) return on;
    struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    if (st == NK_WIDGET_VALID && nk_input_is_mouse_click_in_rect(&ctx->input, NK_BUTTON_LEFT, b)) on = !on;
    float h = U(24), w = U(42), x = b.x + b.w - w, y = b.y + (b.h - h) / 2;
    nk_fill_rect(cv, nk_rect(x, y, w, h), h / 2, on ? C_ACCENT : C_LINE);
    float k = U(18); nk_fill_circle(cv, nk_rect(on ? x + w - k - U(3) : x + U(3), y + U(3), k, k), nk_rgb(255, 255, 255));
    return on;
}
static int is_gpx_ini(const char *f) { return !strncasecmp(f, "gpx", 3) && strlen(f) > 4 && !strcasecmp(f + strlen(f) - 4, ".ini"); }
/* a key missing from a GPx .ini means the module's own default, often not the one the Windows
   GPxPatch shows (the GPxSAT [Sky] options are off when missing): write the value shown */
static void ensure(const char *f, const char *s, const char *k, const char *d) {
    if (d && *d && is_gpx_ini(f) && S.gpx && !has(f, s, k)) put(f, s, k, d);
}
static void W_check(const char *f, const char *s, const char *k, int d, const char *label, const char *hint) {
    char ds[8]; snprintf(ds, sizeof ds, "%d", d); ensure(f, s, k, ds);
    if (g_dry) return;
    int on = atoi(get(f, s, k, ds)) != 0;
    row_begin(label, hint, U(50)); row_push(U(8), U(42), U(30));
    int n = ui_switch(on); if (n != on) puti(f, s, k, n);
    row_end();
}
static void W_check_inv(const char *f, const char *s, const char *k, int d, const char *label, const char *hint) {   /* the switch shows !value */
    if (g_dry) return;
    char ds[8]; snprintf(ds, sizeof ds, "%d", d);
    int on = !(atoi(get(f, s, k, ds)) != 0);
    row_begin(label, hint, U(50)); row_push(U(8), U(42), U(30));
    int n = ui_switch(on); if (n != on) puti(f, s, k, !n);
    row_end();
}
static void W_num(const char *f, const char *s, const char *k, double d, const char *label, const char *hint, double mn, double mx, double step, const char *unit, int fl) {
    char ds[32]; if (fl) { snprintf(ds, sizeof ds, "%g", d); if (!strchr(ds, '.')) strcat(ds, "."); } else snprintf(ds, sizeof ds, "%d", (int)d);
    ensure(f, s, k, ds);
    if (g_dry) return;
    const char *cur = get(f, s, k, ds); double v = *cur ? atof(cur) : d;
    float uw = unit ? text_w(F_SMALL, L(unit), -1) + U(8) : 0;
    row_begin(label, hint, U(150) + uw); row_push(0, U(150), U(30));
    char id[300]; snprintf(id, sizeof id, "##%s|%s|%s", f, s, k);
    if (fl) { float x = (float)v; nk_property_float(ctx, id, (float)mn, &x, (float)mx, (float)step, (float)step); if (x != (float)v) { char b[32]; snprintf(b, sizeof b, "%g", x); if (!strchr(b, '.')) strcat(b, "."); put(f, s, k, b); } }
    else { int x = (int)v; nk_property_int(ctx, id, (int)mn, &x, (int)mx, (int)(step < 1 ? 1 : step), (float)(step < 1 ? 1 : step)); if (x != (int)v) puti(f, s, k, x); }
    if (unit) { row_push(U(156), uw, U(30)); nk_label_colored(ctx, L(unit), NK_TEXT_LEFT, C_MUTED); }
    row_end();
}
typedef struct { const char *v, *label; } opt;
/* a select over n options (labels translated); returns 1 when changed */
static int combo(const char *cur, const opt *o, int n, float w, char *out, size_t outsz) {
    const char *shown = cur; for (int i = 0; i < n; i++) if (!strcasecmp(o[i].v, cur)) { shown = L(o[i].label); break; }
    int changed = 0;
    float h = U(30) * (n < 12 ? n : 12) + U(12);
    if (nk_combo_begin_label(ctx, shown, nk_vec2(w, h))) {
        nk_layout_row_dynamic(ctx, U(28), 1);
        for (int i = 0; i < n; i++) if (nk_combo_item_label(ctx, L(o[i].label), NK_TEXT_LEFT)) { snprintf(out, outsz, "%s", o[i].v); changed = 1; }
        nk_combo_end(ctx);
    }
    return changed;
}
/* the same, showing a given text (e.g. only the file name of a long path) */
static int combo_shown(const char *shown, const opt *o, int n, float w, char *out, size_t outsz) {
    int changed = 0; float h = U(30) * (n < 14 ? n : 14) + U(12);
    if (nk_combo_begin_label(ctx, shown, nk_vec2(w, h))) {
        nk_layout_row_dynamic(ctx, U(28), 1);
        for (int i = 0; i < n; i++) if (nk_combo_item_label(ctx, o[i].label, NK_TEXT_LEFT)) { snprintf(out, outsz, "%s", o[i].v); changed = 1; }
        nk_combo_end(ctx);
    }
    return changed;
}
static void W_sel(const char *f, const char *s, const char *k, const char *d, const char *label, const opt *o, int n, const char *hint, float w) {
    ensure(f, s, k, d);
    if (g_dry) return;
    if (w <= 0) w = U(200);
    row_begin(label, hint, w); row_push(0, w, U(30));
    char nv[512];
    if (combo(get(f, s, k, d), o, n, w, nv, sizeof nv)) put(f, s, k, nv);
    row_end();
}
/* text fields keep their own buffer while being edited */
typedef struct { char id[300]; char buf[1024]; int active; } tbuf;
static tbuf g_tb[64]; static int g_ntb;
static tbuf *tb_get(const char *id) {
    for (int i = 0; i < g_ntb; i++) if (!strcmp(g_tb[i].id, id)) return &g_tb[i];
    tbuf *t = &g_tb[g_ntb < 64 ? g_ntb++ : 63]; memset(t, 0, sizeof *t); snprintf(t->id, sizeof t->id, "%s", id); return t;
}
/* an edit box bound to a value; returns 1 when committed (Enter or leaving the field) */
static int edit(const char *id, const char *cur, char *out, size_t outsz, nk_flags extra) {
    tbuf *t = tb_get(id);
    if (!t->active) snprintf(t->buf, sizeof t->buf, "%s", cur);
    if (getenv("GP3_UI_DEBUG") && ctx->input.mouse.buttons[NK_BUTTON_LEFT].clicked) { struct nk_rect wb = nk_widget_bounds(ctx);
        hlog("edit %s bounds %.0f,%.0f %.0fx%.0f mouse %.0f,%.0f down %d rom %d", id, wb.x, wb.y, wb.w, wb.h, ctx->input.mouse.pos.x, ctx->input.mouse.pos.y,
             ctx->input.mouse.buttons[NK_BUTTON_LEFT].down, (ctx->current->layout->flags & NK_WINDOW_ROM) != 0); }
    nk_flags r = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER | extra, t->buf, (int)sizeof t->buf - 1, nk_filter_default);
    t->active = (r & NK_EDIT_ACTIVE) != 0;
    if (getenv("GP3_UI_DEBUG") && r & ~(nk_flags)NK_EDIT_INACTIVE) hlog("edit %s r=%x buf=%s", id, (unsigned)r, t->buf);
    if ((r & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED)) && strcmp(t->buf, cur)) {
        char *a = t->buf, *e = a + strlen(a); while (*a == ' ') a++; while (e > a && e[-1] == ' ') *--e = 0;
        snprintf(out, outsz, "%s", a); if (r & NK_EDIT_COMMITED) nk_edit_unfocus(ctx); return 1;
    }
    return 0;
}
static void W_text(const char *f, const char *s, const char *k, const char *d, const char *label, const char *hint) {
    ensure(f, s, k, d);
    if (g_dry) return;
    row_begin(label, hint, U(330)); row_push(0, U(330), U(30));
    char id[300], nv[1024]; snprintf(id, sizeof id, "%s|%s|%s", f, s, k);
    if (edit(id, get(f, s, k, d), nv, sizeof nv, 0)) put(f, s, k, nv);
    row_end();
}
static void W_note(const char *text, int warn) {
    if (g_dry) return;
    float lw = g_cw - U(28); int n = wrap_text(NULL, F_TEXT, 0, 0, lw, text, C_TEXT, U(19));
    nk_layout_row_dynamic(ctx, n * U(19) + U(22), 1);
    struct nk_rect r; if (!nk_widget(&r, ctx)) { space(U(12)); return; }
    struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    struct nk_color bg = warn ? nk_rgb(0x3a, 0x33, 0x1e) : nk_rgb(0x3b, 0x22, 0x1f), bd = warn ? nk_rgb(0x7a, 0x63, 0x2c) : nk_rgb(0x8a, 0x35, 0x2a);
    nk_fill_rect(cv, r, U(10), bg); nk_stroke_rect(cv, r, U(10), U(1), bd);
    wrap_text(cv, F_TEXT, r.x + U(14), r.y + U(11), lw, text, C_TEXT, U(19));
    space(U(12));
}
static void notice(const char *t) { W_note(t, 1); }

/* ------------------------------------------------------------------ */
/* folder browser (files of some extensions, or a folder)               */
typedef void (*pick_fn)(const char *win_path, void *ud);
static struct {
    int open, dir_mode; char path[PATH_MAX]; char ext[64]; char title[256]; pick_fn cb; void *ud;
    strv dirs, files; int fresh;
} B;
static int ext_ok(const char *name, const char *list) {
    const char *dot = strrchr(name, '.'); if (!dot) return 0;
    char l[64]; snprintf(l, sizeof l, "%s", list);
    char *sv = NULL; for (char *t = strtok_r(l, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) if (!strcasecmp(dot, t)) return 1;
    return 0;
}
static void browse_load(const char *path) {
    char real[PATH_MAX], ci[PATH_MAX];
#ifdef _WIN32
    if (path[0] == '/' && isalpha((unsigned char)path[1]) && path[2] == ':') path++;   /* "/C:" from the drive list */
    if (!strcmp(path, "/") || !path[0]) {   /* the root: the drives */
        snprintf(B.path, sizeof B.path, "/"); sv_clear(&B.dirs); sv_clear(&B.files);
        DWORD m = GetLogicalDrives(); for (int i = 0; i < 26; i++) if (m & (1u << i)) { char d[4] = { (char)('A' + i), ':', 0 }; sv_add(&B.dirs, d); }
        nk_group_set_scroll(ctx, "blist", 0, 0); return;
    }
#endif
    if (!realpath(path, real)) { if (!(lx_ci_walk("/", path, ci, sizeof ci) && realpath(ci, real))) snprintf(real, sizeof real, "%s", lx_dir()); }
    if (!is_dir(real)) snprintf(real, sizeof real, "%s", lx_dir());
    snprintf(B.path, sizeof B.path, "%s", real); sv_clear(&B.dirs); sv_clear(&B.files);
    DIR *d = opendir(real); struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[PATH_MAX * 2]; snprintf(full, sizeof full, "%s/%s", real, e->d_name);
        if (is_dir(full)) sv_add(&B.dirs, e->d_name); else if (ext_ok(e->d_name, B.ext)) sv_add(&B.files, e->d_name);
    }
    if (d) closedir(d);
    sv_sort(&B.dirs); sv_sort(&B.files);
    nk_group_set_scroll(ctx, "blist", 0, 0);
}
static void to_game(const char *abs, char *out, size_t outsz) {   /* host path -> GP3 path (relative, or absolute: Z:\ on Linux, C:\ on Windows) */
    size_t gl = strlen(lx_dir());
#ifdef _WIN32
    if (!strncasecmp(abs, lx_dir(), gl) && abs[gl] == '/') snprintf(out, outsz, "%s", abs + gl + 1);
    else snprintf(out, outsz, "%s", abs);
#else
    if (!strncmp(abs, lx_dir(), gl) && abs[gl] == '/') snprintf(out, outsz, "%s", abs + gl + 1);
    else { while (abs[0] == '/' && abs[1] == '/') abs++; snprintf(out, outsz, "Z:%s", abs); }
#endif
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';
}
static void from_game(const char *v, char *out, size_t outsz) {
#ifdef _WIN32
    if (v[0] && v[1] == ':') snprintf(out, outsz, "%s", v);
#else
    if ((v[0] == 'z' || v[0] == 'Z') && v[1] == ':') snprintf(out, outsz, "%s", v + 2);
#endif
    else snprintf(out, outsz, "%s/%s", lx_dir(), v);
    for (char *c = out; *c; c++) if (*c == '\\') *c = '/';
}
static void browse(const char *title, const char *ext, const char *start, int dir_mode, pick_fn cb, void *ud) {
    memset(&B.title, 0, sizeof B.title); snprintf(B.title, sizeof B.title, "%s", title ? title : "");
    snprintf(B.ext, sizeof B.ext, "%s", ext ? ext : ".dat"); B.dir_mode = dir_mode; B.cb = cb; B.ud = ud; B.open = 1; B.fresh = 1;
    browse_load(start && *start ? start : lx_dir());
}
static void browse_draw(float W, float H) {
    if (!B.open) return;
    float w = W < U(760) ? W - U(32) : U(720), h = H * 0.84f;
    if (nk_popup_begin(ctx, NK_POPUP_STATIC, "browse", NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR, nk_rect((W - w) / 2, (H - h) / 2, w, h))) {
        struct nk_style *st = &ctx->style; struct nk_vec2 sp = st->window.spacing; st->window.spacing = nk_vec2(U(6), U(6));
        nk_layout_row_begin(ctx, NK_STATIC, U(32), 4);
        char nav[PATH_MAX] = "";
        nk_layout_row_push(ctx, U(90)); if (nk_button_label(ctx, L("Acima"))) { snprintf(nav, sizeof nav, "%s", B.path); size_t nl = strlen(nav); if (nl && nav[nl - 1] == '/' && nl > 1) nav[--nl] = 0; char *sl = strrchr(nav, '/'); if (sl && sl != nav) *sl = 0; else snprintf(nav, sizeof nav, "/"); }
        nk_layout_row_push(ctx, U(140)); if (nk_button_label(ctx, L("Pasta do jogo"))) snprintf(nav, sizeof nav, "%s", lx_dir());
        nk_layout_row_push(ctx, U(140)); if (nk_button_label(ctx, L("Pasta pessoal"))) {
#ifdef _WIN32
            snprintf(nav, sizeof nav, "%s", getenv("USERPROFILE") ? getenv("USERPROFILE") : "C:/"); win_slashes(nav);
#else
            snprintf(nav, sizeof nav, "%s", getenv("HOME") ? getenv("HOME") : "/");
#endif
        }
        nk_layout_row_end(ctx);
        nk_layout_row_dynamic(ctx, U(22), 1);
        char pt[PATH_MAX + 300]; snprintf(pt, sizeof pt, "%s (%s) · %s", L(B.title), B.ext, B.path);
        nk_label_colored(ctx, pt, NK_TEXT_LEFT, C_MUTED);
        nk_layout_row_dynamic(ctx, h - U(32 + 22 + 50 + 30), 1);
        char pick[PATH_MAX] = "";
        if (nk_group_begin(ctx, "blist", NK_WINDOW_BORDER)) {
            nk_layout_row_dynamic(ctx, U(28), 1);
            for (int i = 0; i < B.dirs.n; i++) { char l[PATH_MAX]; snprintf(l, sizeof l, "\xe2\x96\xb6  %s", B.dirs.v[i]);
                if (nk_select_label(ctx, l, NK_TEXT_LEFT, 0)) snprintf(nav, sizeof nav, "%s%s%s", B.path, B.path[strlen(B.path) - 1] != '/' ? "/" : "", B.dirs.v[i]); }
            for (int i = 0; i < B.files.n; i++) {
                if (B.dir_mode) nk_label_colored(ctx, B.files.v[i], NK_TEXT_LEFT, C_MUTED);
                else if (nk_select_label(ctx, B.files.v[i], NK_TEXT_LEFT, 0)) snprintf(pick, sizeof pick, "%s%s%s", B.path, B.path[strlen(B.path) - 1] != '/' ? "/" : "", B.files.v[i]);
            }
            if (!B.dirs.n && !B.files.n) nk_label_colored(ctx, L("Pasta vazia."), NK_TEXT_LEFT, C_MUTED);
            nk_group_end(ctx);
        }
        nk_layout_row_begin(ctx, NK_STATIC, U(34), 3);
        nk_layout_row_push(ctx, w - U(2 * 150 + 2 * 14 + 2 * 6 + 4));
        nk_spacing(ctx, 1);
        int use = 0;
        nk_layout_row_push(ctx, U(150)); if (B.dir_mode) { if (nk_button_label(ctx, L("Usar esta pasta"))) use = 1; } else nk_spacing(ctx, 1);
        nk_layout_row_push(ctx, U(150)); if (nk_button_label(ctx, L("Cancelar"))) { B.open = 0; nk_popup_close(ctx); }
        nk_layout_row_end(ctx);
        st->window.spacing = sp;
        if (B.fresh) { B.fresh = 0; nav[0] = 0; pick[0] = 0; use = 0; }   /* the click that opened it is not a choice */
        if (nav[0]) browse_load(nav);
        if (pick[0] || use) {
            char g[PATH_MAX]; to_game(pick[0] ? pick : B.path, g, sizeof g);
            B.open = 0; nk_popup_close(ctx);
            if (B.cb) B.cb(g, B.ud);
        }
        nk_popup_end(ctx);
    } else B.open = 0;
}

/* a small question box: a name, OK / Cancel */
typedef void (*ask_fn)(const char *text, void *ud);
static struct { int open; char q[512]; char buf[256]; ask_fn cb; void *ud; int focus; } A;
static void ask(const char *q, ask_fn cb, void *ud) { A.open = 1; snprintf(A.q, sizeof A.q, "%s", q); A.buf[0] = 0; A.cb = cb; A.ud = ud; A.focus = 3; }
static void ask_draw(float W, float H) {   /* its own window: text fields lose focus inside popups */
    if (!A.open) return;
    float w = U(480), h = U(176);
    if (nk_begin(ctx, "ask", nk_rect((W - w) / 2, (H - h) / 2, w, h), NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        nk_window_set_focus(ctx, "ask");
        struct nk_style *st = &ctx->style; struct nk_vec2 sp = st->window.spacing, pd = st->window.padding;
        st->window.spacing = nk_vec2(U(8), U(8));
        nk_layout_row_dynamic(ctx, U(14), 1); nk_spacing(ctx, 1);
        nk_layout_row_begin(ctx, NK_STATIC, U(40), 1); nk_layout_row_push(ctx, w - U(36));
        nk_label_wrap(ctx, A.q); nk_layout_row_end(ctx);
        if (A.focus) { nk_edit_focus(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER); A.focus--; }
        nk_layout_row_begin(ctx, NK_STATIC, U(32), 2); nk_layout_row_push(ctx, U(10)); nk_spacing(ctx, 1); nk_layout_row_push(ctx, w - U(40));
        nk_flags r = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, A.buf, (int)sizeof A.buf - 1, nk_filter_default);
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, U(34), 3); nk_layout_row_push(ctx, U(10)); nk_spacing(ctx, 1);
        nk_layout_row_push(ctx, (w - U(48)) / 2);
        int ok = nk_button_label(ctx, "OK") || (r & NK_EDIT_COMMITED), cancel = 0;
        nk_layout_row_push(ctx, (w - U(48)) / 2);
        if (nk_button_label(ctx, L("Cancelar"))) cancel = 1;
        nk_layout_row_end(ctx);
        if (nk_input_is_key_pressed(&ctx->input, NK_KEY_TEXT_RESET_MODE)) cancel = 1;
        st->window.spacing = sp; st->window.padding = pd;
        if (cancel) A.open = 0;
        else if (ok && A.buf[0]) { A.open = 0; char t[256]; snprintf(t, sizeof t, "%s", A.buf); if (A.cb) A.cb(t, A.ud); }
    }
    nk_end(ctx);
}

/* ------------------------------------------------------------------ */
/* file pickers bound to a key                                          */
typedef struct { const char *re1, *re2; const char *pre[2]; const char *start; const char *where; } root_t;
/* liveries: gp3jams\liveries or gp3jamsh\liveries; main: \main; jams: gp3jams(h)\; tech: gp3jamsh\techjam; game: anything */
static const root_t ROOTS[] = {
    { "gp3jams\\liveries\\", "gp3jamsh\\liveries\\", { "gp3jams\\liveries\\", "gp3jamsh\\liveries\\" }, "gp3jamsh/liveries", "gp3jams\\liveries / gp3jamsh\\liveries" },
    { "gp3jams\\main\\", "gp3jamsh\\main\\", { "gp3jams\\main\\", "gp3jamsh\\main\\" }, "gp3jamsh/main", "gp3jams\\main / gp3jamsh\\main" },
    { "gp3jams\\", "gp3jamsh\\", { "gp3jams\\", "gp3jamsh\\" }, "gp3jams/genjam", "gp3jams" },
    { "gp3jamsh\\techjam\\", NULL, { "gp3jamsh\\techjam\\", NULL }, "gp3jamsh/techjam", "gp3jamsh\\techjam" },
    { NULL, NULL, { NULL, NULL }, "", "" },
};
enum { R_LIV, R_MAIN, R_JAMS, R_TECH, R_GAME };
static const char *strip_root(int root, const char *w) {   /* NULL when outside the root */
    const root_t *R = &ROOTS[root];
    if (!R->re1) return w;
    if ((w[0] == 'z' || w[0] == 'Z') && w[1] == ':') return NULL;
    for (int i = 0; i < 2; i++) { const char *p = i ? R->re2 : R->re1; if (p && !strncasecmp(w, p, strlen(p))) {
        const char *rest = w + strlen(p);
        if (root == R_TECH && strchr(rest, '\\')) return NULL;
        return rest; } }
    return NULL;
}
static int value_exists(int root, const char *v) {
    if (!*v) return 1;
    const root_t *R = &ROOTS[root]; char p[PATH_MAX];
    if (!R->re1) return file_exists_win(v);
    for (int i = 0; i < 2; i++) if (R->pre[i]) { snprintf(p, sizeof p, "%s%s", R->pre[i], v); if (file_exists_win(p)) return 1; }
    return 0;
}
/* cache of "does it exist" (asked every frame otherwise) */
static struct { char k[600]; int ok; Uint32 t; } g_ex[512]; static int g_nex;
static int exists_cached(int root, const char *v) {
    char k[600]; snprintf(k, sizeof k, "%d|%s", root, v); Uint32 now = SDL_GetTicks();
    for (int i = 0; i < g_nex; i++) if (!strcmp(g_ex[i].k, k)) { if (now - g_ex[i].t < 3000) return g_ex[i].ok; g_ex[i].ok = value_exists(root, v); g_ex[i].t = now; return g_ex[i].ok; }
    int i = g_nex < 512 ? g_nex++ : (int)(now % 512);
    snprintf(g_ex[i].k, sizeof g_ex[i].k, "%s", k); g_ex[i].ok = value_exists(root, v); g_ex[i].t = now; return g_ex[i].ok;
}
typedef struct { char f[256], s[128], k[128]; int root; char label[256]; } pickctx;
static pickctx g_pc;
static void pick_done(const char *w, void *ud) {
    pickctx *p = ud; const char *v = strip_root(p->root, w);
    if (!v) { status(Lf("Escolha um arquivo dentro de {0}.", ROOTS[p->root].where, 0, 0)); return; }
    put(p->f, p->s, p->k, v);
}
/* the file box + button inside a row (x, w relative to the control area) */
static void file_box(const char *f, const char *s, const char *k, int root, const char *ext, const char *label, float x, float w, int compact) {
    const char *cur = get(f, s, k, "");
    float bw = compact ? U(30) : U(96);
    row_push(x, w - bw - U(6), U(30));
    char id[300], nv[1024]; snprintf(id, sizeof id, "%s|%s|%s", f, s, k);
    int miss = !exists_cached(root, cur);
    if (miss) { nk_style_push_color(ctx, &ctx->style.edit.border_color, C_WARN); nk_style_push_color(ctx, &ctx->style.edit.text_normal, C_WARN); }
    if (edit(id, cur, nv, sizeof nv, 0)) put(f, s, k, nv[0] ? nv : NULL);
    if (miss) { nk_style_pop_color(ctx); nk_style_pop_color(ctx); }
    row_push(x + w - bw, bw, U(30));
    if (nk_button_label(ctx, compact ? "..." : L("Procurar…"))) {
        snprintf(g_pc.f, sizeof g_pc.f, "%s", f); snprintf(g_pc.s, sizeof g_pc.s, "%s", s); snprintf(g_pc.k, sizeof g_pc.k, "%s", k); g_pc.root = root;
        char start[PATH_MAX];
        if (*cur) { const root_t *R = &ROOTS[root]; char wp[PATH_MAX]; snprintf(wp, sizeof wp, "%s%s", R->pre[1] ? R->pre[1] : R->pre[0] ? R->pre[0] : "", cur);
            from_game(wp, start, sizeof start); char *sl = strrchr(start, '/'); if (sl) *sl = 0; }
        else snprintf(start, sizeof start, "%s/%s", lx_dir(), ROOTS[root].start);
        browse(label, ext, start, 0, pick_done, &g_pc);
    }
}
static __attribute__((unused)) void W_file(const char *f, const char *s, const char *k, const char *label, int root, const char *ext, const char *hint) {
    if (g_dry) return;
    row_begin(label, hint, U(400)); file_box(f, s, k, root, ext, label, 0, U(400), 0); row_end();
}

/* ------------------------------------------------------------------ */
/* tabs                                                                 */
static const char *gpx_dir_ini(void) { return "gpxpatch.ini"; }
static void shortcuts_banner(void) {
    if (S.shortcuts || g_dry) return;
    const char *t = Lf("Crie os atalhos “{0}” e “GPxPatch” no menu de aplicativos e na área de trabalho, para abrir o jogo com dois cliques.", S.is2000 ? "Grand Prix 3 2000" : "Grand Prix 3", 0, 0);
    float lw = g_cw - U(200); int n = wrap_text(NULL, F_TEXT, 0, 0, lw, t, C_TEXT, U(19));
    float h = n * U(19) + U(26); if (h < U(52)) h = U(52);
    struct nk_rect a = space_row(h, g_cw, 2); struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    nk_fill_rect(cv, a, U(10), nk_rgb(0x3b, 0x22, 0x1f)); nk_stroke_rect(cv, a, U(10), U(1), nk_rgb(0x8a, 0x35, 0x2a));
    wrap_text(cv, F_TEXT, a.x + U(14), a.y + (h - n * U(19)) / 2, lw, t, C_TEXT, U(19));
    nk_layout_space_push(ctx, nk_rect(g_cw - U(160), (h - U(32)) / 2, U(146), U(32)));
    nk_style_push_style_item(ctx, &ctx->style.button.normal, nk_style_item_color(C_ACCENT));
    nk_style_push_color(ctx, &ctx->style.button.text_normal, nk_rgb(255, 255, 255));
    if (nk_button_label(ctx, L("Criar atalhos"))) { int ok = lx_make_shortcuts(); S.shortcuts = lx_shortcuts_exist(); status(ok ? L("Atalhos criados no menu de aplicativos e na área de trabalho.") : L("Não foi possível criar os atalhos.")); }
    nk_style_pop_color(ctx); nk_style_pop_style_item(ctx);
    nk_layout_space_end(ctx); space(U(14));
}
static const char *MODS[][3] = { { "gpxset", "GPxSet", "temporadas, carros, física, chuva, corrida rápida" }, { "gpxtrack", "GPxTrack", "pistas da comunidade e limites estendidos" },
    { "gpxlap", "GPxLap", "tempos (ATL), log, acerto no carro, quicksave" }, { "gpxsat", "GPxSAT", "gráficos de TV" }, { "gpxcinfo", "GPxCInfo", "painéis de informação do carro" },
    { "gpxsound", "GPxSound", "sons 3D do motor e do box" }, { "gpxivibe", "intelliVIBE", "vibração (hardware antigo)" } };
static void tab_gpx(void) {
    const char *P = gpx_dir_ini(), *R = "gp3rt.ini";
    shortcuts_banner();
    group_begin("Ao abrir o jogo", NULL);
#ifndef _WIN32
    W_check(R, "game", "gpxpatch", 1, "O atalho do jogo inicia com o GPxPatch", "Os botões lá embaixo escolhem só para esta vez.");
    W_check_inv(R, "game", "intro_videos", 1, "Pular introdução", "Os vídeos de abertura (Esc também pula).");
#endif
    W_check(P, "Misc", "SkipStartupMenu", 0, "Pular o menu inicial", "Vai direto ao menu principal.");
    W_check(P, "Misc", "SkipWeatherMenu", 0, "Pular o menu de clima", "Útil na corrida rápida.");
    group_end();
    group_begin("Extensões ativadas", NULL);
    for (int i = 0; i < 7; i++) W_check(P, "Mod", MODS[i][0], strcmp(MODS[i][0], "gpxivibe") ? 1 : 0, MODS[i][1], MODS[i][2]);
    group_end();
    group_begin("Jogo", NULL);
    static const opt ep[] = { { "0", "Desligado" }, { "1", "Ligado" }, { "2", "Ligado depois de apertar Enter na pausa" } };
    W_sel(P, "Misc", "ExtendedPause", "0", "Modo de pausa estendido", ep, 3, "Na pausa dá para trocar de câmera e usar as teclas do GPx.", U(330));
    W_text(P, "Misc", "HostIPAddress", "", "Endereço IP do host (multiplayer)", "Vazio = procura na rede local.");
    group_end();
    group_begin("Debug", NULL);
#ifndef _WIN32
    W_check(R, "game", "skip_asserts", 1, "Iniciar em modo debug", "Ignora as verificações internas do GP3, como o modo debug do GPxPatch: necessário para pistas que “só funcionam em modo debug”.");
#else
    (void)R;
#endif
    static const opt vb[] = { { "0", "Normal" }, { "1", "Detalhado" }, { "2", "Muito detalhado" } };
    W_sel(P, "Debug", "Verbose", "0", "Detalhes do registro (gpxpdata/log)", vb, 3, NULL, U(200));
    group_end();
}
static opt *opts_from(const strv *a, const char *first_v, const char *first_l, int *n) {   /* (frame-lifetime) */
    static opt o[8][2100]; static int k; opt *r = o[k++ & 7]; int m = 0;
    if (first_v) { r[m].v = first_v; r[m].label = first_l; m++; }
    for (int i = 0; i < a->n && m < 2100; i++) { r[m].v = a->v[i]; r[m].label = a->v[i]; m++; }
    *n = m; return r;
}
/* GPxSet: the tab itself, or (GP3 2000) the season editor / the car set editor inside it */
static int g_sub;            /* 0 the GPxSet tab, 1 season editor, 2 car set editor */
static int g_sea_tab, g_cs_tab, g_cs_slot;
static void season_page(void); static void carset_page(void); static void season_select_row(void);
static const char *cs_key(char *k, size_t n); static void open_carset(const char *n);
/* a switch and a file box on one row (like a checkbox + path in the Windows GPxPatch) */
static void W_switch_file(const char *f, const char *se, const char *ke, int d, const char *label, const char *hint, const char *s, const char *k, int root, const char *ext) {
    char ds[8]; snprintf(ds, sizeof ds, "%d", d); ensure(f, se, ke, ds);
    if (g_dry) return;
    row_begin(label, hint, U(520));
    int on = atoi(get(f, se, ke, ds)) != 0; row_push(0, U(42), U(30)); int n = ui_switch(on); if (n != on) puti(f, se, ke, n);
    file_box(f, s, k, root, ext, label, U(54), U(466), 0);
    row_end();
}
static void W_file_al(const char *f, const char *s, const char *k, const char *label, int root, const char *ext, const char *hint) {   /* aligned with W_switch_file */
    if (g_dry) return;
    row_begin(label, hint, U(520)); file_box(f, s, k, root, ext, label, U(54), U(466), 0); row_end();
}
/* a switch and a select on one row */
static void W_switch_sel(const char *f, const char *se, const char *ke, int d, const char *label, const char *hint, const char *s, const char *k, const char *dv, const opt *o, int n, float w) {
    char ds[8]; snprintf(ds, sizeof ds, "%d", d); ensure(f, se, ke, ds); ensure(f, s, k, dv);
    if (g_dry) return;
    row_begin(label, hint, U(54) + w);
    int on = atoi(get(f, se, ke, ds)) != 0; row_push(0, U(42), U(30)); int nn = ui_switch(on); if (nn != on) puti(f, se, ke, nn);
    row_push(U(54), w, U(30)); char nv[512]; if (combo(get(f, s, k, dv), o, n, w, nv, sizeof nv)) put(f, s, k, nv);
    row_end();
}
static void tab_set(void) {
    const char *F = "gpxset.ini";
    if (g_sub == 1 && S.is2000) { season_page(); return; }
    if (g_sub == 2 && S.is2000) { carset_page(); return; }
    if (S.is2000) { group_begin("Temporada", NULL); season_select_row(); group_end(); }
    group_begin("Carros", NULL);
    static const opt tp[] = { { "0", "Comportamento antigo" }, { "1", "Só potência" }, { "2", "Só aderência" }, { "3", "Potência e aderência" } };
    W_switch_sel(F, "Misc", "TeamPower", 0, "Força do carro dependendo da equipe", "O seu carro fica com a performance do carro do computador da mesma equipe. “Potência e aderência” é o recomendado.", "Misc", "TeamPowerType", "3", tp, 4, U(260));
    W_switch_file(F, "PerfManager", "Enable", 0, "Arquivo de performance", "Arquivo .txt feito com o GP3Ed / TeamEditor.", "PerfManager", "Path", R_GAME, ".txt");
    W_switch_file(F, "CarshapeManager", "Enable", 0, "Carshape", "Arquivo .cs3 feito com o editor de carros do Paul Hoad.", "CarshapeManager", "Carshape", R_GAME, ".cs3");
    W_switch_file(F, "PhysicsManager", "Enable", 0, "Física", "Arquivo .g3p do GP3 Advanced Physics Editor.", "PhysicsManager", "Path", R_GAME, ".g3p");
    W_file_al(F, "PhysicsManager", "TorquePath", "Torque", R_GAME, ".ini", "Arquivo .ini exportado pelo Torque Editor.");
    group_end();
    group_begin("Gráficos", NULL);
    { strv sk = { 0 }; for (int i = 0; i < S.skies.n; i++) sv_add(&sk, S.skies.v[i]); if (!sv_has(&sk, "skya")) sv_add(&sk, "skya"); sv_sort(&sk);
      int n; opt *o = opts_from(&sk, NULL, NULL, &n);
      W_sel(F, "Misc", "skya", "skya", "Prefixo das imagens do céu", o, n, "Imagens skyaNN.bmp em gp3jamsh\\techjam (céu de dia, entardecer, noite...).", U(260));
      sv_clear(&sk); free(sk.v); }
    { strv tm = { 0 }; if (!sv_has(&S.tarmacs, "tarmacmap.bmp")) sv_add(&tm, "tarmacmap.bmp"); for (int i = 0; i < S.tarmacs.n; i++) sv_add(&tm, S.tarmacs.v[i]);
      int n; opt *o = opts_from(&tm, NULL, NULL, &n);
      W_sel(F, "Misc", "tarmac", "tarmacmap.bmp", "Bitmap da pista", o, n, "O asfalto, em gp3jamsh\\techjam.", U(260));
      sv_clear(&tm); free(tm.v); }
    if (S.is2000) W_check(F, "Misc", "DisableProgressBar", 0, "Desabilitar a barra de progressão", NULL);
    group_end();
    group_begin("Sessões", NULL);
    W_num(F, "Misc", "QuickraceLength", 5, "Tamanho da corrida rápida", "% da distância real.", 1, 100, 1, "%", 0);
    W_num(F, "Misc", "FreePracticeCars", 1, "Carros no treino livre", NULL, 1, 22, 1, NULL, 0);
    W_num(F, "Misc", "ConsecutiveCars", 13, "Carros do computador na pista ao mesmo tempo", "Nas sessões que não são corrida.", 1, 22, 1, NULL, 0);
    W_num(F, "Misc", "RainChance", 50, "Probabilidade de chuva", "Quando o clima é aleatório.", 0, 100, 1, "%", 0);
    group_end();
}

/* GPxTrack */
static const char *SLOTS_113[] = { "Austrália", "Brasil", "Argentina", "San Marino", "Espanha", "Mônaco", "Canadá", "França", "Grã-Bretanha", "Áustria", "Alemanha", "Hungria", "Bélgica", "Itália", "Luxemburgo", "Japão" };
static const char *SLOTS_2000[] = { "Austrália", "Brasil", "San Marino", "Grã-Bretanha", "Espanha", "Europa", "Mônaco", "Canadá", "França", "Áustria", "Alemanha", "Hungria", "Bélgica", "Itália", "EUA", "Japão", "Malásia" };
static int copy_set(const char *from, const char *to) {   /* gpxpdata\from -> gpxpdata\to (new) */
    char a[PATH_MAX], b[PATH_MAX]; if (!lx_file_path(to, b, sizeof b) || !access(b, F_OK)) return 0;
    size_t n = 0; char *t = lx_file_path(from, a, sizeof a) ? lx_read_file(a, &n) : NULL;
    FILE *f = fopen(b, "wb"); if (!f) { free(t); return 0; }
    if (t) fwrite(t, 1, n, f); else fputs("[TrackFiles]\r\n", f);
    fclose(f); free(t); return 1;
}
static void clean_name(const char *in, char *out, size_t outsz) {
    size_t w = 0; for (const char *s = in; *s && w + 1 < outsz; s++) if (!strchr("\\/:*?\"<>|\t[]=", *s)) out[w++] = *s;
    out[w] = 0; while (w && out[w - 1] == ' ') out[--w] = 0;
    char *s = out; while (*s == ' ') s++; if (s != out) memmove(out, s, strlen(s) + 1);
    size_t l = strlen(out); if (l > 4 && !strcasecmp(out + l - 4, ".set")) out[l - 4] = 0;
}
static int name_taken(const char *n) { return sv_has(&S.sets, n) || sv_has(&S.seasons, n) || sv_has(&S.carsets, n); }
static void new_trackset(const char *t, void *ud) {
    (void)ud; char n[256]; clean_name(t, n, sizeof n); if (!n[0]) return; strcat(n, ".set");
    flush();
    const char *SET = get("gpxtrack.ini", "TrackManager", "TrackSet", "tracks.set"); if (!*SET) SET = "tracks.set";
    if (!copy_set(SET, n)) { status(L("Já existe um conjunto com esse nome.")); return; }
    put("gpxtrack.ini", "TrackManager", "TrackSet", n); flush(); scan();
    status(Lf("Conjunto “{0}” criado. As mudanças agora vão para ele.", n, 0, 0));
}
static int g_slot_pick;
static const char *cur_set(void) { const char *v = get("gpxtrack.ini", "TrackManager", "TrackSet", "tracks.set"); return *v ? v : "tracks.set"; }
static void slot_done(const char *w, void *ud) {
    (void)ud; char SET[256]; snprintf(SET, sizeof SET, "%s", cur_set());
    char k[16]; snprintf(k, sizeof k, "f1ct%02d", g_slot_pick + 1);
    put(SET, "TrackFiles", k, w);
    if (!has("gpxtrack.ini", "TrackManager", "TrackSet")) put("gpxtrack.ini", "TrackManager", "TrackSet", SET);
}
static void empty_trackset(const char *t, void *ud) {
    (void)ud; char n[256], b[PATH_MAX]; clean_name(t, n, sizeof n); if (!n[0]) return; strcat(n, ".set");
    if (!lx_file_path(n, b, sizeof b) || !access(b, F_OK) || name_taken(n)) { status(L("Já existe um conjunto com esse nome.")); return; }
    FILE *f = fopen(b, "wb"); if (!f) return; fputs("[TrackFiles]\r\n", f); fclose(f);
    ini_forget(n); flush(); put("gpxtrack.ini", "TrackManager", "TrackSet", n); flush(); scan();
    status(Lf("Conjunto “{0}” criado. As mudanças agora vão para ele.", n, 0, 0));
}
/* a .set chosen with "Abrir…": in gpxpdata it is just selected; elsewhere it is imported into
   gpxpdata, its paths (relative to the .set itself) turned into paths the game finds */
static void open_trackset(const char *g, void *ud) {
    (void)ud; char host[PATH_MAX]; from_game(g, host, sizeof host);
    const char *gd = lx_gpxdir(); const char *base = strrchr(host, '/'); base = base ? base + 1 : host;
    char rd[PATH_MAX], rg[PATH_MAX]; snprintf(rd, sizeof rd, "%s", host); char *sl = strrchr(rd, '/'); if (sl) *sl = 0;
    if (gd[0] && realpath(rd, rg) && realpath(gd, rd) && !strcmp(rg, rd)) {
        if (lx_set_kind(host) != 't') { status(L("Esse arquivo .set não é um conjunto de pistas.")); return; }
        ini_forget(base); put("gpxtrack.ini", "TrackManager", "TrackSet", base); flush(); scan(); return;
    }
    size_t n = 0; char *t = lx_read_file(host, &n); if (!t) return;
    if (!strstr(t, "[TrackFiles]") && !strstr(t, "[trackfiles]")) { free(t); status(L("Esse arquivo .set não é um conjunto de pistas.")); return; }
    char name[256]; snprintf(name, sizeof name, "%s", base); char dst[PATH_MAX];
    for (int i = 2; lx_file_path(name, dst, sizeof dst) && !access(dst, F_OK) && i < 100; i++) snprintf(name, sizeof name, "%.*s (%d).set", (int)(strlen(base) - 4), base, i);
    FILE *f = lx_file_path(name, dst, sizeof dst) ? fopen(dst, "wb") : NULL; if (!f) { free(t); return; }
    char setdir[PATH_MAX]; snprintf(setdir, sizeof setdir, "%s", host); sl = strrchr(setdir, '/'); if (sl) *sl = 0;
    char *sv = NULL;
    for (char *ln = strtok_r(t, "\r\n", &sv); ln; ln = strtok_r(NULL, "\r\n", &sv)) {
        char *eq = strchr(ln, '=');
        if (eq && ln[0] != '[' && ln[0] != ';' && eq[1] && !(eq[2] == ':') && eq[1] != '\\') {
            char abs[PATH_MAX * 2], rel[PATH_MAX]; snprintf(abs, sizeof abs, "%s/%s", setdir, eq + 1);
            for (char *c = abs; *c; c++) if (*c == '\\') *c = '/';
            char ci[PATH_MAX]; if (lx_ci_walk("/", abs + 1, ci, sizeof ci)) snprintf(abs, sizeof abs, "%s", ci);
            to_game(abs, rel, sizeof rel); *eq = 0; fprintf(f, "%s=%s\r\n", ln, rel);
        } else fprintf(f, "%s\r\n", ln);
    }
    fclose(f); free(t);
    ini_forget(name); put("gpxtrack.ini", "TrackManager", "TrackSet", name); flush(); scan();
    status(Lf("Conjunto “{0}” copiado para a pasta gpxpdata e escolhido.", name, 0, 0));
}
static int g_del_set; static Uint32 g_del_t;
static void tab_track(void) {
    const char *F = "gpxtrack.ini";
    char SET[256]; snprintf(SET, sizeof SET, "%s", cur_set());
    if (!g_dry && S.circuit_inis.n == 1) notice(Lf("A pasta circuits tem um único arquivo .ini ({0}). O GPx usa esse .ini para todas as pistas modificadas dessa pasta, e por isso várias etapas aparecem com o mesmo nome. Deixe cada pista da comunidade (.dat, .ini e arquivos dela) numa pasta própria, por exemplo “pistas\\Donington”, e escolha o .dat aqui com “Procurar…”.", S.circuit_inis.v[0], 0, 0));
    static const opt tl[] = { { "0", "8 km (original)" }, { "1", "10 km" }, { "2", "20 km" } }, ml[] = { { "0", "99 (original)" }, { "1", "125" }, { "2", "253" } };
    ensure(F, "Misc", "ExtendTrackLength", "0"); ensure(F, "Misc", "ExtendMaxRaceLaps", "0");
    ensure(F, "TrackManager", "CatchMenuStrings", "1"); ensure(F, "TrackManager", "CatchRecords", "0");
    if (S.is2000) ensure(F, "TrackManager", "IgnoreCarSet", "0");
    if (g_dry) return;
    float pad = U(16), half = (g_cw - 2 * pad - U(24)) / 2;
    /* options: the two switches on the left, the two limits on the right (like the Windows GPxPatch) */
    group_begin("Opções", NULL);
    {
        const char *lab[3][2] = { { "Mostrar nos menus o nome, país e recordes das pistas", "Comprimento máximo da pista" }, { "Capturar recordes", "Voltas máximas na corrida" },
                                  { "Ignorar as pistas da temporada e dos carsets", NULL } };
        const char *key[3] = { "CatchMenuStrings", "CatchRecords", "IgnoreCarSet" }, *lim[2] = { "ExtendTrackLength", "ExtendMaxRaceLaps" };
        for (int r = 0; r < (S.is2000 ? 3 : 2); r++) {
            struct nk_rect a = space_row(U(34), g_cw, 4); struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
            int on = atoi(get(F, "TrackManager", key[r], r ? "0" : "1"));
            nk_layout_space_push(ctx, nk_rect(pad, U(2), U(48), U(30))); int nv = ui_switch(on); if (nv != on) puti(F, "TrackManager", key[r], nv);
            wrap_text(cv, F_TEXT, a.x + pad + U(58), a.y + U(7), half - U(60), L(lab[r][0]), C_TEXT, U(19));
            if (r == 2) { struct nk_rect tr = nk_rect(a.x + pad, a.y, half, U(34));
                if (nk_input_is_mouse_hovering_rect(&ctx->input, tr)) nk_tooltip(ctx, L("Normalmente a pista escolhida num carset (ou o conjunto de pistas da temporada) tem prioridade sobre esta aba."));
                nk_layout_space_end(ctx); continue; }
            float x2 = pad + half + U(24);
            wrap_text(cv, F_TEXT, a.x + x2, a.y + U(7), half - U(170), L(lab[r][1]), C_TEXT, U(19));
            nk_layout_space_push(ctx, nk_rect(x2 + half - U(160), U(2), U(160), U(30)));
            char v[64]; if (combo(get(F, "Misc", lim[r], "0"), r ? ml : tl, 3, U(160), v, sizeof v)) put(F, "Misc", lim[r], v);
            nk_layout_space_end(ctx);
        }
    }
    group_end();

    group_begin("Seleção de pistas", NULL);
    {
        struct nk_rect a = space_row(U(40), g_cw, 6); (void)a;
        strv l = { 0 }; if (!sv_has(&S.sets, SET)) sv_add(&l, SET); for (int i = 0; i < S.sets.n; i++) sv_add(&l, S.sets.v[i]);
        int n; opt *o = opts_from(&l, NULL, NULL, &n); char nv[256];
        float x = pad, bw = U(118), cw = g_cw - 2 * pad - 4 * (bw + U(6));
        nk_layout_space_push(ctx, nk_rect(x, U(5), cw, U(30)));
        if (combo(SET, o, n, cw, nv, sizeof nv)) { put(F, "TrackManager", "TrackSet", nv); flush(); g_del_set = 0; }
        sv_clear(&l); free(l.v); x += cw + U(6);
        nk_layout_space_push(ctx, nk_rect(x, U(5), bw, U(30))); x += bw + U(6);
        if (nk_button_label(ctx, L("Abrir…"))) { char st[PATH_MAX]; snprintf(st, sizeof st, "%s", lx_gpxdir()[0] ? lx_gpxdir() : lx_dir()); browse("Conjunto de pistas", ".set", st, 0, open_trackset, NULL); }
        nk_layout_space_push(ctx, nk_rect(x, U(5), bw, U(30))); x += bw + U(6);
        if (nk_button_label(ctx, L("Novo…"))) ask(L("Nome do novo conjunto de pistas (ex.: temporada2006):"), empty_trackset, NULL);
        nk_layout_space_push(ctx, nk_rect(x, U(5), bw, U(30))); x += bw + U(6);
        if (nk_button_label(ctx, L("Salvar como…"))) ask(L("Nome do novo conjunto de pistas (ex.: temporada1998):"), new_trackset, NULL);
        nk_layout_space_push(ctx, nk_rect(x, U(5), bw, U(30)));
        if (g_del_set && SDL_GetTicks() - g_del_t > 4000) g_del_set = 0;
        if (nk_button_label(ctx, L(g_del_set ? "Confirmar" : "Excluir"))) {
            if (!g_del_set) { g_del_set = 1; g_del_t = SDL_GetTicks(); }
            else { char pth[PATH_MAX]; g_del_set = 0; flush();
                if (lx_file_path(SET, pth, sizeof pth) && !unlink(pth)) { ini_forget(SET); scan();
                    put(F, "TrackManager", "TrackSet", S.sets.n ? S.sets.v[0] : "tracks.set"); flush();
                    status(Lf("Conjunto “{0}” excluído.", SET, 0, 0)); } } }
        nk_layout_space_end(ctx);
    }
    {
        const char **names = S.is2000 ? SLOTS_2000 : SLOTS_113; int ns = S.is2000 ? 17 : 16, rows = (ns + 1) / 2;
        static char (*lbl)[PATH_MAX + 8]; static int nl; if (!lbl || nl < S.dats.n + 2) { free(lbl); nl = S.dats.n + 2; lbl = calloc((size_t)nl, sizeof *lbl); }
        static opt *o; static int no; if (!o || no < S.dats.n + 2) { free(o); no = S.dats.n + 2; o = calloc((size_t)no, sizeof *o); }
        int hover = -1; struct nk_rect hr = { 0 };
        for (int r = 0; r < rows; r++) {
            struct nk_rect a = space_row(U(32), g_cw, 6); struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
            for (int c = 0; c < 2; c++) {
                int i = c * rows + r; if (i >= ns) continue;
                float x0 = pad + c * (half + U(24));
                char k[16], num[8], orig[32]; snprintf(k, sizeof k, "f1ct%02d", i + 1); snprintf(num, sizeof num, "%02d", i + 1); snprintf(orig, sizeof orig, "F1CT%02d.dat", i + 1);
                struct nk_rect nr = nk_rect(a.x + x0, a.y + U(1), U(34), U(30));
                draw_text(cv, F_TEXT, nr.x + U(4), a.y + U(7), U(30), num, C_MUTED);
                if (nk_input_is_mouse_hovering_rect(&ctx->input, nr)) { hover = i; hr = nr; }
                const char *cur = get(SET, "TrackFiles", k, "");
                int n = 0; snprintf(lbl[n], sizeof lbl[n], "%s  %s", orig, L("(original)")); o[n].v = ""; o[n].label = lbl[n]; n++;
                if (*cur && !sv_has(&S.dats, cur)) { const char *b = strrchr(cur, '\\'); snprintf(lbl[n], sizeof lbl[n], "%s   (%.*s)", b ? b + 1 : cur, b ? (int)(b - cur) : 0, cur); o[n].v = cur; o[n].label = lbl[n]; n++; }
                for (int j = 0; j < S.dats.n; j++) { const char *v = S.dats.v[j], *b = strrchr(v, '\\');
                    if (b) snprintf(lbl[n], sizeof lbl[n], "%s   (%.*s)", b + 1, (int)(b - v), v); else snprintf(lbl[n], sizeof lbl[n], "%s", v);
                    o[n].v = v; o[n].label = lbl[n]; n++; }
                const char *base = strrchr(cur, '\\'); char shown[PATH_MAX];
                if (*cur) snprintf(shown, sizeof shown, "%s", base ? base + 1 : cur); else snprintf(shown, sizeof shown, "%s  %s", orig, L("(original)"));
                float cw = half - U(40) - U(42); char nv[PATH_MAX];
                nk_layout_space_push(ctx, nk_rect(x0 + U(40), U(1), cw, U(30)));
                if (combo_shown(shown, o, n, cw > U(520) ? cw : U(520), nv, sizeof nv)) { put(SET, "TrackFiles", k, nv[0] ? nv : NULL); if (!has(F, "TrackManager", "TrackSet")) put(F, "TrackManager", "TrackSet", SET); }
                nk_layout_space_push(ctx, nk_rect(x0 + U(40) + cw + U(6), U(1), U(36), U(30)));
                struct nk_rect fb = nk_widget_bounds(ctx); int fclick = nk_button_label(ctx, "");
                { float fx = fb.x + fb.w / 2 - U(8), fy = fb.y + fb.h / 2 - U(6); struct nk_color fc = C_MUTED;   /* a folder */
                  nk_fill_rect(cv, nk_rect(fx, fy, U(7), U(3)), U(1), fc); nk_fill_rect(cv, nk_rect(fx, fy + U(2), U(16), U(11)), U(2), fc); }
                if (fclick) {
                    g_slot_pick = i; char st[PATH_MAX] = "";
                    if (*cur) { from_game(cur, st, sizeof st); char *sl = strrchr(st, '/'); if (sl) *sl = 0; }
                    if (!st[0] || !is_dir(st)) { if (!lx_find_ci(lx_dir(), "circuits", st, sizeof st)) snprintf(st, sizeof st, "%s", lx_dir()); }
                    browse("Pista", ".dat", st, 0, slot_done, NULL);
                }
            }
            nk_layout_space_end(ctx);
        }
        if (hover >= 0 && nk_input_is_mouse_hovering_rect(&ctx->input, hr)) nk_tooltip(ctx, Lf("Slot {0} · {1} no jogo original", (char[3]){ (char)('0' + (hover + 1) / 10), (char)('0' + (hover + 1) % 10), 0 }, L(names[hover]), 0));
    }
    group_end();
}

static const char *FONTS_DEF[] = { "courier", "din-bold", "ATL2004", "digital2004", "futura", "manana" };
static opt *font_opts(int *n) {
    static strv fn; sv_clear(&fn);
    ifile *F = ini("fonts.ini"); for (isec *x = F->secs; x; x = x->next) sv_add(&fn, x->name);
    for (int i = 0; i < 6; i++) if (!sv_has(&fn, FONTS_DEF[i])) sv_add(&fn, FONTS_DEF[i]);
    return opts_from(&fn, NULL, NULL, n);
}
static void W_font(const char *f, const char *s, const char *k, const char *d, const char *label) { int n; opt *o = font_opts(&n); W_sel(f, s, k, d, label, o, n, NULL, U(200)); }
static void W_color(const char *f, const char *s, const char *k, const char *d, const char *label) {
    ensure(f, s, k, d);
    if (g_dry) return;
    unsigned long n = strtoul(get(f, s, k, d), NULL, 16); unsigned a = (n >> 24) & 255;
    struct nk_colorf c = { ((n >> 16) & 255) / 255.f, ((n >> 8) & 255) / 255.f, (n & 255) / 255.f, 1 };
    row_begin(label, NULL, U(120)); row_push(0, U(120), U(30));
    if (nk_combo_begin_color(ctx, nk_rgb_cf(c), nk_vec2(U(240), U(260)))) {
        nk_layout_row_dynamic(ctx, U(200), 1);
        struct nk_colorf c2 = nk_color_picker(ctx, c, NK_RGB);
        if (c2.r != c.r || c2.g != c.g || c2.b != c.b) {
            char b[16]; snprintf(b, sizeof b, "0x%08x", (unsigned)((a << 24) | ((unsigned)(c2.r * 255 + .5f) << 16) | ((unsigned)(c2.g * 255 + .5f) << 8) | (unsigned)(c2.b * 255 + .5f)));
            put(f, s, k, b);
        }
        nk_combo_end(ctx);
    }
    row_end();
}
static void tab_lap(void) {
    const char *F = "gpxlap.ini";
    group_begin("Geral", NULL);
    W_check(F, "Authentication", "Enable", 1, "Habilitar autenticação", "Marca os tempos de volta para provar que não houve trapaça.");
    W_check(F, "Setup", "Enable", 0, "Mudanças de acerto dentro do carro", "Tecla L (acerto no box: \\ [ ]).");
    W_check(F, "SaveRestore", "Enable", 0, "Salvar/restaurar o estado do carro", "Backspace.");
    W_check(F, "Quicksave", "Auto", 0, "Autosave", NULL);
    W_check(F, "Misc", "BestSplit", 0, "Mostrar a melhor parcial na qualificação", NULL);
    W_check(F, "Misc", "PitExtView", 0, "Visão externa no pitlane", NULL);
    W_check(F, "Misc", "FixPitstopBug", 0, "Corrigir o bug do pit stop", NULL);
    W_check(F, "Misc", "HideReplayMenu", 0, "Esconder o menu de replay", NULL);
    W_num(F, "Misc", "RaceOverDuration", 6, "Tempo da tela de fim de corrida", NULL, 0, 60, 1, "s", 0);
    group_end();
    group_begin("Tabela de tempos (ATL, tecla 9)", NULL);
    W_font(F, "ATL", "Font", "ATL2004", "Fonte");
    W_color(F, "ATL", "PanelColor", "0x00000000", "Cor do painel");
    W_num(F, "ATL", "NumFastestCars", 3, "Carros mais rápidos destacados", NULL, 0, 22, 1, NULL, 0);
    group_end();
    group_begin("Log (tecla 2)", NULL);
    W_font(F, "Log", "Font", "courier", "Fonte");
    W_num(F, "Log", "PanelSize", 14, "Linhas do painel", NULL, 1, 60, 1, NULL, 0);
    W_num(F, "Log", "PanelAlpha", 112, "Transparência do painel", "0 = invisível, 255 = opaco.", 0, 255, 1, NULL, 0);
    group_end();
    group_begin("Acerto (tecla L)", NULL);
    W_num(F, "Setup", "DisplayDuration", 3000, "Tempo na tela", NULL, 0, 60000, 100, "ms", 0);
    W_check(F, "Setup", "ShowWear", 1, "Mostrar desgaste", NULL);
    W_check(F, "Setup", "ShowWearInQualify", 0, "Mostrar desgaste na qualificação", NULL);
    W_check(F, "Setup", "ShowWearInRace", 0, "Mostrar desgaste na corrida", NULL);
    group_end();
    group_begin("Registro de voltas", NULL);
    W_check(F, "LapLog", "Enable", 0, "Registro de texto das voltas", NULL);
    W_text(F, "LapLog", "Path", "laps.log", "Arquivo", NULL);
    W_check(F, "LapLog", "Append", 0, "Adicionar ao arquivo", NULL);
    group_end();
    group_begin("Temperatura do motor", NULL);
    W_num(F, "EngineTemperature", "Start", 93, "Inicial", NULL, -50, 300, 1, "°C", 0);
    W_num(F, "EngineTemperature", "IdleLow", 24, "Parado (mínima)", NULL, -50, 300, 1, "°C", 0);
    W_num(F, "EngineTemperature", "IdleHigh", 93, "Parado (máxima)", NULL, -50, 300, 1, "°C", 0);
    W_num(F, "EngineTemperature", "Maximum", 125, "Máxima", NULL, -50, 300, 1, "°C", 0);
    W_num(F, "EngineTemperature", "ChangeSpeed", 16, "Velocidade de mudança", NULL, 0, 1000, 1, NULL, 0);
    group_end();
    group_begin("Distância RCR", NULL);
    W_num(F, "RCR", "DistCockpit", 3840, "Cockpit", NULL, 0, 100000, 64, NULL, 0);
    W_num(F, "RCR", "DistOnBoard", 3840, "Câmera de bordo", NULL, 0, 100000, 64, NULL, 0);
    W_num(F, "RCR", "DistChase", 7680, "Câmera de perseguição", NULL, 0, 100000, 64, NULL, 0);
    W_num(F, "RCR", "DistTrack", 7680, "Câmera da pista", NULL, 0, 100000, 64, NULL, 0);
    group_end();
}
static const opt SATMODES[] = { { "0", "Clássico" }, { "1", "Digital" }, { "2", "Digital 2004" }, { "3", "Digital 2010" } };
static void tab_sat(void) {
    const char *F = "gpxsat.ini";
    group_begin("Gráficos de TV", NULL);
    W_sel(F, "Misc", "Mode", "3", "Modalidade", SATMODES, 4, NULL, U(200));
    W_check(F, "Misc", "ShowInCockpit", 0, "Mostrar no cockpit", "Sem isso os gráficos só aparecem nas câmeras externas.");
    W_check(F, "Misc", "ShowMicroTime", 0, "Microtime ligado ao iniciar", "Tecla M.");
    W_num(F, "Misc", "SpeedTrapWidth", 50, "Tempo das informações de parcial na tela", "Quanto antes e depois de cada ponto (50 a 100).", 0, 300, 1, NULL, 0);
    group_end();
    group_begin("Substituir as mensagens no céu", NULL);
    W_check(F, "Misc", "ReplaceSkyMessagesWhenShowInCockpitIsOff", 0, "Substituir também quando “Mostrar no cockpit” está desligado", NULL);
    W_check(F, "Sky", "ReplaceFastestLap", 1, "Volta mais rápida", NULL);
    W_check(F, "Sky", "ReplaceOutOfRace", 1, "Fora da corrida", NULL);
    W_check(F, "Sky", "ReplaceIsInPits", 1, "Nos boxes", NULL);
    W_check(F, "Sky", "ReplaceHasWonRace", 1, "Corrida vencida", NULL);
    W_check(F, "Sky", "ReplaceRidingWith", 1, "Pilotando com", NULL);
    W_check(F, "Sky", "ReplaceBlackFlag", 1, "Bandeira preta", NULL);
    group_end();
    group_begin("Microtime", NULL);
    W_num(F, "MicroTime", "PosX", 10, "Posição X", NULL, 0, 4000, 1, "px", 0);
    W_num(F, "MicroTime", "PosY", 10, "Posição Y", NULL, 0, 4000, 1, "px", 0);
    group_end();
    group_begin("Fontes", NULL);
    W_font(F, "Font", "FontClassic", "manana", "Clássico");
    W_font(F, "Font", "FontDigital", "futura", "Digital");
    W_font(F, "Font", "FontDigital2004", "digital2004", "Digital 2004");
    W_font(F, "Font", "FontDigital2010", "din-bold", "Digital 2010");
    W_font(F, "Font", "Font2D", "courier", "Textos 2D");
    group_end();
    group_begin("Carros por página na classificação", NULL);
    W_num(F, "Classic", "CarsPerPage", 8, "Clássico", NULL, 1, 22, 1, NULL, 0);
    W_num(F, "Digital", "CarsPerPage", 6, "Digital", NULL, 1, 22, 1, NULL, 0);
    W_num(F, "Digital2004", "CarsPerPage", 8, "Digital 2004", NULL, 1, 22, 1, NULL, 0);
    W_num(F, "Digital2010", "CarsPerPage", 5, "Digital 2010", NULL, 1, 22, 1, NULL, 0);
    group_end();
    group_begin("Mapa da pista (tecla 8)", NULL);
    W_check(F, "TrackMap", "Enable", 1, "Mapa da pista", NULL);
    W_num(F, "TrackMap", "TrackMapSize", 256, "Tamanho", "Em 1024x768; ajustado para a resolução.", 64, 1024, 1, "px", 0);
    W_num(F, "TrackMap", "TrackMapPosX", 0, "Posição X", NULL, 0, 4000, 1, "px", 0);
    W_num(F, "TrackMap", "TrackMapPosY", 0, "Posição Y", NULL, 0, 4000, 1, "px", 0);
    W_check(F, "TrackMap", "ShowCars", 1, "Mostrar os carros", NULL);
    W_check(F, "TrackMap", "ShowCCs", 1, "Mostrar os carros do computador", NULL);
    W_check(F, "TrackMap", "ShowYellowZones", 1, "Mostrar as zonas de bandeira amarela", NULL);
    W_check(F, "TrackMap", "AutoShowEnabled", 1, "Aparecer sozinho com bandeira amarela", NULL);
    W_check(F, "TrackMap", "TrackMapBackground", 0, "Fundo semitransparente", NULL);
    group_end();
}
static void W_wav(const char *f, const char *s, const char *k, const char *d, const char *label) {
    strv l = { 0 }; for (int i = 0; i < S.wavs.n; i++) { const char *v = S.wavs.v[i]; sv_add(&l, v); }
    int n; opt *o = opts_from(&l, NULL, NULL, &n); W_sel(f, s, k, d, label, o, n, NULL, U(260)); sv_clear(&l); free(l.v);
}
static void tab_sound(void) {
    const char *F = "gpxsound.ini";
    group_begin("Som 3D do motor dos outros carros", NULL);
    W_check(F, "Engine", "Enabled", 1, "Ativar som 3D do motor", NULL);
    W_check(F, "Engine", "DisableGPxSound3D", 1, "Desabilitar o falso som 3D do GPx", NULL);
    W_wav(F, "Engine", "Wave", "engine.wav", "Arquivo .wav do motor");
    W_num(F, "Engine", "MinDistance", 4, "Distância mínima", NULL, 0, 1000, 0.5, "m", 1);
    W_num(F, "Engine", "MaxDistance", 200, "Distância máxima", NULL, 1, 10000, 1, "m", 1);
    W_num(F, "Engine", "Revs2PitchFactor", 1.2, "Frequência: fator por RPM", NULL, -100, 100, 0.05, NULL, 1);
    W_num(F, "Engine", "Revs2PitchOffset", 4000, "Frequência: soma", NULL, -100000, 100000, 10, "Hz", 0);
    W_num(F, "Engine", "Revs2VolumeFactor", 0.2, "Volume: fator por RPM", NULL, -100, 100, 0.05, NULL, 1);
    W_num(F, "Engine", "Revs2VolumeOffset", -2000, "Volume: soma", NULL, -10000, 10000, 10, "1/100 dB", 0);
    group_end();
    group_begin("Sirene do box", NULL);
    W_check(F, "Siren", "Enabled", 1, "Ativar som 3D da sirene do box", NULL);
    W_wav(F, "Siren", "Wave", "pithorn.wav", "Arquivo .wav da sirene");
    W_num(F, "Siren", "MinDistance", 5, "Distância mínima", NULL, 0, 1000, 0.5, "m", 1);
    W_num(F, "Siren", "MaxDistance", 500, "Distância máxima", NULL, 1, 10000, 1, "m", 1);
    group_end();
    group_begin("Sons de eventos", NULL);
    W_check(F, "EventSound", "Enabled", 1, "Ativar os sons 2D de eventos", NULL);
    W_num(F, "EventSound", "Volume", 0, "Volume geral", NULL, -10000, 0, 100, "1/100 dB", 0);
    W_text(F, "EventSound", "SoundSet", "soundset.ini", "Arquivo de eventos (em gpxpdata/snd)", NULL);
    group_end();
    group_begin("Avançado", NULL);
    W_num(F, "Listener", "DistanceFactor", 1, "Fator de distância", NULL, 0, 100, 0.1, NULL, 1);
    W_num(F, "Listener", "RolloffFactor", 1, "Fator de atenuação", NULL, 0, 100, 0.1, NULL, 1);
    W_num(F, "Listener", "DopplerFactor", 1, "Fator Doppler", NULL, 0, 100, 0.1, NULL, 1);
    group_end();
}
static void g_scroll_reset_req(void);
/* GPxCInfo: panels and the panel editor (gpxpdata\pnl\...\*.ini) */
static char g_pnl[PATH_MAX];       /* panel being edited: "pnl\<path from gpxcinfo.ini>" */
static const char *const CTRL_TYPES[][2] = {
    { "Speed", "Velocímetro" }, { "Revs", "Conta-giros" }, { "Gear", "Marcha" }, { "SuggestedGear", "Marcha sugerida" }, { "GearChanges", "Trocas de marcha" },
    { "Fuel", "Combustível" }, { "EngineTemp", "Temperatura do motor" }, { "LongAcc", "Aceleração (frente/trás)" }, { "LatAcc", "Aceleração lateral" },
    { "Throttle", "Acelerador" }, { "Brake", "Freio" }, { "Steer", "Direção" }, { "Lap", "Volta" }, { "Position", "Posição" },
    { "BestLapTime", "Melhor volta" }, { "GameTime", "Tempo de jogo" }, { "PitTimer", "Cronômetro do box" }, { "Pit", "Box (pit)" },
    { "BlueFlag", "Bandeira azul" }, { "YellowFlag", "Bandeira amarela" }, { "HotseatState", "Hotseat" },
    { "HelpF1", "Ajuda F1" }, { "HelpF2", "Ajuda F2" }, { "HelpF3", "Ajuda F3" }, { "HelpF4", "Ajuda F4" }, { "HelpF5", "Ajuda F5" },
    { "HelpF6", "Ajuda F6" }, { "HelpF7", "Ajuda F7" }, { "HelpF8", "Ajuda F8" }, { "HelpF9", "Ajuda F9" },
};
#define NCTRL ((int)(sizeof CTRL_TYPES / sizeof *CTRL_TYPES))
static const char *KNOWN_KEYS[] __attribute__((unused)) = { "Enable", "X", "Y", "XOffset", "YOffset", "Mode", "Color", "CutOff", "Offset", "Length", "Scale", "AngleStart", "AngleEnd",
    "Clockwise", "WeightLeft", "WeightTop", "WeightRight", "WeightBottom", "Threshold", "ThresholdHi", "Min", "Max", "Decimals", "Prefix", "Postfix",
    "Align", "TextColor", "TextAlpha", "TextColor2", "TextAlpha2", "ColorFixed", "EnableText", "UseImages", "UsePosForImages", "Font", "Sign",
    "ShowMillis", "ShowTotalLaps", "ShowLapsLeft", "RaceMode", "NameLength", "CountDown", "Minutes", "TopFreezeDuration", "ShowDiffWhenFrozen",
    "TextColorSlower", "TextColorFaster", "TextColorBusy", "TextColorOff", "TextColorDisabled", "TextColorDisabledOn", "UseBitmap", "AvgFrames",
    "Path", "Width", "Height", "Alpha", "SrcX", "SrcY", "SrcWidth", "SrcHeight", "ShowForTrack", "ShowForTeam", "ShowForCar", "ControlSection", "ControlType" };
static const char *ctrl_name(const char *sec) {   /* "RevsControl" -> "Conta-giros"; "PitImage1" -> ... */
    static char b[128];
    for (int i = 0; i < NCTRL; i++) { size_t l = strlen(CTRL_TYPES[i][0]);
        if (!strncasecmp(sec, CTRL_TYPES[i][0], l) && !strcasecmp(sec + l, "Control")) return L(CTRL_TYPES[i][1]); }
    if (!strcasecmp(sec, "Misc")) return L("Geral do painel");
    if (!strcasecmp(sec, "Filter")) return L("Quando mostrar");
    if (!strcasecmp(sec, "Images")) return L("Imagens");
    if (!strcasecmp(sec, "Range")) return L("Trecho da pista / sessão");
    const char *im = ci_strstr(sec, "Image");
    if (im && isdigit((unsigned char)im[5])) { snprintf(b, sizeof b, "%s %s", L("Imagem"), im + 5); if (im != sec) { char t[64]; snprintf(t, sizeof t, "%.*s", (int)(im - sec), sec); snprintf(b, sizeof b, "%s %s (%s)", L("Imagem"), im + 5, t); } return b; }
    return sec;
}
static int key_is_bool(const char *k) {
    static const char *bk[] = { "Enable", "Clockwise", "Negate", "Scaling", "DrawTransparentPanels", "ShowInReplay", "ShowInPits", "ShowInSAT", "ShowForPlayer",
        "UseImages", "UsePosForImages", "EnableText", "ColorFixed", "Sign", "ShowMillis", "ShowTotalLaps", "ShowLapsLeft", "CountDown", "UseBitmap",
        "ShowDiffWhenFrozen", "CountOnTop", "Underline", "StrikeOut", "Italic", "Filtering", "RaceMode", NULL };
    for (int i = 0; bk[i]; i++) if (!strcasecmp(bk[i], k)) return 1;
    return 0;
}
static const char *key_label(const char *k) {
    static const char *kl[][2] = { { "Enable", "Ligado" }, { "X", "Posição X" }, { "Y", "Posição Y" }, { "Color", "Cor" }, { "TextColor", "Cor do texto" },
        { "TextAlpha", "Opacidade do texto (0 a 1)" }, { "Alpha", "Opacidade (0 a 1)" }, { "Path", "Imagem (.tga)" }, { "Width", "Largura" }, { "Height", "Altura" },
        { "Scale", "Escala" }, { "Length", "Comprimento do ponteiro" }, { "Offset", "Deslocamento do ponteiro" }, { "AngleStart", "Ângulo inicial" },
        { "AngleEnd", "Ângulo final" }, { "Clockwise", "Sentido horário" }, { "Mode", "Modo" }, { "Min", "Mínimo" }, { "Max", "Máximo" },
        { "Decimals", "Casas decimais" }, { "Prefix", "Texto antes" }, { "Postfix", "Texto depois" }, { "Align", "Alinhamento" }, { "Font", "Fonte" },
        { "Left", "Esquerda" }, { "Top", "Topo" }, { "Right", "Direita" }, { "Bottom", "Base" }, { "DrawTransparentPanels", "Painéis transparentes" },
        { "Scaling", "Ajustar à resolução" }, { "Count", "Quantidade" }, { "CountOnTop", "Na frente dos controles" }, { "ShowInReplay", "No replay" },
        { "ShowInPits", "Nos boxes" }, { "ShowInSAT", "Com os gráficos de TV" }, { "ShowForPlayer", "Para o jogador" }, { "SessionMode", "Sessões" },
        { "Camera", "Câmeras" }, { "Name", "Nome" }, { "EnableText", "Mostrar o texto" }, { "ShowForTrack", "Só nestas pistas (números)" },
        { "TopFreezeDuration", "Congelar no máximo (ms)" }, { "TextColor2", "Segunda cor" }, { "TextAlpha2", "Segunda opacidade (0 a 1)" },
        { "WeightLeft", "Peso esquerda" }, { "WeightTop", "Peso topo" }, { "WeightRight", "Peso direita" }, { "WeightBottom", "Peso base" },
        { "Threshold", "Limiar" }, { "ThresholdHi", "Limiar superior" }, { "XOffset", "Deslocamento X" }, { "YOffset", "Deslocamento Y" },
        { "AvgFrames", "Média de quadros" }, { "UseImages", "Usar imagens" }, { "UsePosForImages", "Imagens na posição do item" },
        { "TextColorOff", "Cor quando desligado" }, { "TextColorBusy", "Cor quando ocupado" }, { "TextColorFaster", "Cor quando mais rápido" },
        { "TextColorSlower", "Cor quando mais lento" }, { "TextColorDisabled", "Cor quando desativado" }, { "TextColorDisabledOn", "Cor quando sempre ligado" },
        { "ShowTotalLaps", "Mostrar o total de voltas" }, { "ShowLapsLeft", "Mostrar as voltas restantes" }, { "RaceMode", "Modo corrida" },
        { "NameLength", "Letras do nome" }, { "Sign", "Sinal" }, { "ShowMillis", "Mostrar milésimos" }, { "ShowDiffWhenFrozen", "Mostrar a diferença" },
        { "CountDown", "Contagem regressiva" }, { "Minutes", "Começar em (minutos)" }, { "SrcX", "Origem X" }, { "SrcY", "Origem Y" },
        { "SrcWidth", "Largura na origem" }, { "SrcHeight", "Altura na origem" }, { "ShowForTeam", "Só para a equipe" }, { "ShowForCar", "Só para o carro" },
        { "Underline", "Sublinhado" }, { NULL, NULL } };
    for (int i = 0; kl[i][0]; i++) if (!strcasecmp(kl[i][0], k)) return L(kl[i][1]);
    return k;
}
static void pnl_dir(char *out, size_t n) {   /* "pnl\Lo2kTheme" for "pnl\Lo2kTheme\Counter.ini" */
    snprintf(out, n, "%s", g_pnl); char *b = strrchr(out, '\\'); if (b) *b = 0;
}
/* one value of a panel, with the widget its key suggests */
static char g_pnl_pick_sec[128], g_pnl_pick_key[64];
static void pnl_pick_done(const char *w, void *ud) {
    (void)ud; char dir[PATH_MAX], pre[PATH_MAX]; pnl_dir(dir, sizeof dir);
    snprintf(pre, sizeof pre, "gpxpdata\\%s\\", dir);
    if (!strncasecmp(w, pre, strlen(pre))) put(g_pnl, g_pnl_pick_sec, g_pnl_pick_key, w + strlen(pre));
    else { const char *b = strrchr(w, '\\'); put(g_pnl, g_pnl_pick_sec, g_pnl_pick_key, b ? b + 1 : w); status(L("A imagem precisa estar na pasta do painel (ou em gpxpdata\\gfx): foi usado só o nome do arquivo.")); }
}
static void pnl_value(const char *sec, const char *k, const char *v) {
    const char *F = g_pnl;
    if (key_is_bool(k)) {
        row_begin(key_label(k), strcmp(key_label(k), k) ? k : NULL, U(50)); row_push(U(8), U(42), U(30));
        int on = atoi(v) != 0, n = ui_switch(on); if (n != on) puti(F, sec, k, n);
        row_end(); return;
    }
    if (ci_strstr(k, "Color") && !ci_strstr(k, "Fixed")) {
        int hex = !strncasecmp(v, "0x", 2); unsigned long n = strtoul(v, NULL, hex ? 16 : 10);
        unsigned r, g, b, a = (unsigned)(n >> 24) & 255;
        if (hex) { r = (n >> 16) & 255; g = (n >> 8) & 255; b = n & 255; } else { r = n & 255; g = (n >> 8) & 255; b = (n >> 16) & 255; }   /* decimal: Windows COLORREF */
        struct nk_colorf c = { r / 255.f, g / 255.f, b / 255.f, 1 };
        row_begin(key_label(k), strcmp(key_label(k), k) ? k : NULL, U(120)); row_push(0, U(120), U(30));
        if (nk_combo_begin_color(ctx, nk_rgb_cf(c), nk_vec2(U(240), U(260)))) {
            nk_layout_row_dynamic(ctx, U(200), 1);
            struct nk_colorf c2 = nk_color_picker(ctx, c, NK_RGB);
            if (c2.r != c.r || c2.g != c.g || c2.b != c.b) {
                unsigned R = (unsigned)(c2.r * 255 + .5f), G2 = (unsigned)(c2.g * 255 + .5f), B2 = (unsigned)(c2.b * 255 + .5f); char nb[24];
                if (hex) snprintf(nb, sizeof nb, "0x%08x", (a << 24) | (R << 16) | (G2 << 8) | B2); else snprintf(nb, sizeof nb, "%u", (a << 24) | (B2 << 16) | (G2 << 8) | R);
                put(F, sec, k, nb);
            }
            nk_combo_end(ctx);
        }
        row_end(); return;
    }
    if (!strcasecmp(k, "Path")) {
        row_begin(key_label(k), strcmp(key_label(k), k) ? k : NULL, U(400));
        float bw = U(96); row_push(0, U(400) - bw - U(6), U(30));
        char id[300], nv[1024]; snprintf(id, sizeof id, "%s|%s|%s", F, sec, k);
        int miss = 0;
        if (*v) { char dir[PATH_MAX], a1[PATH_MAX * 2], a2[PATH_MAX]; pnl_dir(dir, sizeof dir);
            snprintf(a1, sizeof a1, "gpxpdata\\%s\\%s", dir, v); snprintf(a2, sizeof a2, "gpxpdata\\gfx\\%s", v);
            miss = !exists_cached(R_GAME, a1) && !exists_cached(R_GAME, a2); }
        if (miss) { nk_style_push_color(ctx, &ctx->style.edit.border_color, C_WARN); nk_style_push_color(ctx, &ctx->style.edit.text_normal, C_WARN); }
        if (edit(id, v, nv, sizeof nv, 0)) put(F, sec, k, nv);
        if (miss) { nk_style_pop_color(ctx); nk_style_pop_color(ctx); }
        row_push(U(400) - bw, bw, U(30));
        if (nk_button_label(ctx, L("Procurar…"))) { snprintf(g_pnl_pick_sec, sizeof g_pnl_pick_sec, "%s", sec); snprintf(g_pnl_pick_key, sizeof g_pnl_pick_key, "%s", k);
            char dir[PATH_MAX], st[PATH_MAX], rel[PATH_MAX]; pnl_dir(dir, sizeof dir); snprintf(rel, sizeof rel, "gpxpdata\\%s", dir); from_game(rel, st, sizeof st);
            browse(key_label(k), ".tga,.bmp", st, 0, pnl_pick_done, NULL); }
        row_end(); return;
    }
    if (!strcasecmp(k, "Font")) {
        static strv fn; sv_clear(&fn);
        char dir[PATH_MAX], fp[PATH_MAX]; pnl_dir(dir, sizeof dir); snprintf(fp, sizeof fp, "%s\\fonts.ini", dir);
        char hp[PATH_MAX]; if (lx_file_path(fp, hp, sizeof hp)) for (isec *x = ini(fp)->secs; x; x = x->next) sv_add(&fn, x->name);
        for (isec *x = ini("fonts.ini")->secs; x; x = x->next) if (!sv_has(&fn, x->name)) sv_add(&fn, x->name);
        int n; opt *o = opts_from(&fn, NULL, NULL, &n); char nv[128];
        row_begin(key_label(k), strcmp(key_label(k), k) ? k : NULL, U(220)); row_push(0, U(220), U(30));
        if (combo(v, o, n, U(220), nv, sizeof nv)) put(F, sec, k, nv);
        row_end(); return;
    }
    char *e; double d = strtod(v, &e);
    int numeric = *v && (*e == 0 || (e[0] == 'm' && !e[1]) || (e[0] == 'l' && !e[1]));
    if (numeric && !*e) {
        int fl = strchr(v, '.') != NULL;
        row_begin(key_label(k), strcmp(key_label(k), k) ? k : NULL, U(150)); row_push(0, U(150), U(30));
        char id[300]; snprintf(id, sizeof id, "##%s|%s|%s", F, sec, k);
        if (fl) { float x = (float)d; nk_property_float(ctx, id, -100000.f, &x, 100000.f, 0.05f, 0.01f); if (x != (float)d) { char b2[32]; snprintf(b2, sizeof b2, "%g", x); if (!strchr(b2, '.')) strcat(b2, "."); put(F, sec, k, b2); } }
        else { int x = (int)d; nk_property_int(ctx, id, -100000, &x, 100000, 1, 1); if (x != (int)d) puti(F, sec, k, x); }
        row_end(); return;
    }
    row_begin(key_label(k), strcmp(key_label(k), k) ? k : NULL, U(330)); row_push(0, U(330), U(30));
    char id[300], nv[1024]; snprintf(id, sizeof id, "%s|%s|%s", F, sec, k);
    if (edit(id, v, nv, sizeof nv, 0)) put(F, sec, k, nv);
    row_end();
}
static char g_pnl_del[128];
static void pnl_new_done(const char *t, void *ud) {
    (void)ud; char n[200]; clean_name(t, n, sizeof n); size_t l = strlen(n); if (l > 4 && !strcasecmp(n + l - 4, ".ini")) n[l - 4] = 0; if (!n[0]) return;
    char rel[256]; snprintf(rel, sizeof rel, "%s.ini", n); char nm[300]; snprintf(nm, sizeof nm, "pnl\\%s", rel);
    char hp[PATH_MAX]; if (lx_file_path(nm, hp, sizeof hp) && !access(hp, F_OK)) { status(L("Já existe um arquivo com esse nome.")); return; }
    put(nm, "Misc", "Version", "3"); put(nm, "Misc", "Left", "0"); put(nm, "Misc", "Top", "0"); put(nm, "Misc", "Right", "1024"); put(nm, "Misc", "Bottom", "768");
    put(nm, "Misc", "Font", "courier"); put(nm, "Misc", "TextColor", "0x00ffffff"); put(nm, "Misc", "TextAlpha", "1."); put(nm, "Misc", "DrawTransparentPanels", "1");
    put(nm, "Filter", "Enable", "1"); put(nm, "Filter", "ShowForPlayer", "1"); put(nm, "Filter", "ShowInPits", "1");
    put(nm, "Images", "Enable", "0"); put(nm, "Images", "Count", "0");
    const char *F = "gpxcinfo.ini"; int k = 1; char kk[16]; for (;; k++) { snprintf(kk, sizeof kk, "Path%d", k); if (!*get(F, "Panel", kk, "")) break; }
    put(F, "Panel", kk, rel); snprintf(kk, sizeof kk, "Enable%d", k); put(F, "Panel", kk, "1");
    flush(); scan(); snprintf(g_pnl, sizeof g_pnl, "%s", nm);
}
/* item types, in GPxCInfo's order (Type=N) */
static const char *const ITEM_TYPES[][2] = {
    { "Gear", "Marcha" }, { "Revs", "Rotação (RPM)" }, { "Speed", "Velocidade" }, { "Fuel", "Combustível" }, { "EngineTemp", "Temperatura da água" },
    { "Lap", "Volta" }, { "Position", "Posição" }, { "GapFront", "Diferença para o da frente" }, { "GapBehind", "Diferença para o de trás" }, { "Pit", "Chamada para o box" },
    { "LapTime", "Tempo da volta" }, { "BestLapTime", "Melhor volta" }, { "SessionTime", "Tempo da sessão" },
    { "HelpF1", "Ajuda F1" }, { "HelpF2", "Ajuda F2" }, { "HelpF3", "Ajuda F3" }, { "HelpF4", "Ajuda F4" }, { "HelpF5", "Ajuda F5" }, { "HelpF6", "Ajuda F6" },
    { "HelpF7", "Ajuda F7" }, { "HelpF8", "Ajuda F8" }, { "HelpF9", "Ajuda F9" }, { "Steer", "Direção" }, { "Throttle", "Acelerador" }, { "Brake", "Freio" },
    { "Name", "Nome do piloto" }, { "PO", "Uso do processador (PO)" }, { "Label", "Texto fixo" }, { "SuggestedGear", "Marcha sugerida" }, { "GameTime", "Tempo de jogo" },
    { "LongAcc", "Aceleração (frente/trás)" }, { "LatAcc", "Aceleração lateral" }, { "BlueFlag", "Bandeira azul" }, { "YellowFlag", "Bandeira amarela" },
    { "Warning", "Aviso (falha)" }, { "HotseatState", "Hotseat" }, { "GearChanges", "Trocas de marcha" }, { "PitTimer", "Cronômetro do box" },
};
#define NITEMT ((int)(sizeof ITEM_TYPES / sizeof *ITEM_TYPES))
static int item_count(void) { return geti(g_pnl, "Items", "Count", 0); }
static const char *item_at(int i) { char k[16]; snprintf(k, sizeof k, "Item%02d", i + 1); return get(g_pnl, "Items", k, ""); }
static void item_set(int i, const char *v) { char k[16]; snprintf(k, sizeof k, "Item%02d", i + 1); put(g_pnl, "Items", k, v); }
static void sec_delete(const char *sec) {
    isec *x = sec_find(ini(g_pnl), sec, 0); if (!x) return;
    while (x->keys) put(g_pnl, sec, x->keys->k, NULL);
}
static void pnl_add_item(int type) {
    const char *base = ITEM_TYPES[type][0]; char sec[64];
    for (int n = 0;; n++) { if (n) snprintf(sec, sizeof sec, "%s%d", base, n); else snprintf(sec, sizeof sec, "%s", base);
        if (!sec_find(ini(g_pnl), sec, 0) || !ini(g_pnl)->secs) break; }
    int c = item_count(); puti(g_pnl, "Items", "Count", c + 1); item_set(c, sec);
    puti(g_pnl, sec, "Type", type); put(g_pnl, sec, "Enable", "1"); put(g_pnl, sec, "Name", type == 27 ? "Texto" : "");
    put(g_pnl, sec, "EnableText", "1"); put(g_pnl, sec, "X", "100"); put(g_pnl, sec, "Y", "100"); put(g_pnl, sec, "Align", "0");
    put(g_pnl, sec, "Decimals", "0"); put(g_pnl, sec, "Prefix", ""); put(g_pnl, sec, "Postfix", ""); put(g_pnl, sec, "ColorFixed", "0");
    put(g_pnl, sec, "ControlType", "0");
    status(Lf("Item “{0}” adicionado no fim da lista. Ajuste a posição abaixo.", L(ITEM_TYPES[type][1]), 0, 0));
}
static void pnl_del_item(int i) {
    char sec[128]; snprintf(sec, sizeof sec, "%s", item_at(i)); int c = item_count();
    for (int j = i; j < c - 1; j++) { char v[128]; snprintf(v, sizeof v, "%s", item_at(j + 1)); item_set(j, v); }
    char k[16]; snprintf(k, sizeof k, "Item%02d", c); put(g_pnl, "Items", k, NULL); puti(g_pnl, "Items", "Count", c - 1);
    int used = 0; for (int j = 0; j < c - 1; j++) if (!strcasecmp(item_at(j), sec)) used = 1;
    if (!used) { char cs[160]; sec_delete(sec); snprintf(cs, sizeof cs, "%sControl", sec); sec_delete(cs);
        for (int n = 0; n < 8; n++) { snprintf(cs, sizeof cs, "%sControlImage%d", sec, n); sec_delete(cs); } }
}
static void pnl_move_item(int i, int d) {
    int j = i + d; if (j < 0 || j >= item_count()) return;
    char a[128], b2[128]; snprintf(a, sizeof a, "%s", item_at(i)); snprintf(b2, sizeof b2, "%s", item_at(j)); item_set(i, b2); item_set(j, a);
}
static void pnl_add_image(void) {
    int n = 1; char sec[32]; for (;; n++) { snprintf(sec, sizeof sec, "Image%d", n); if (!sec_find(ini(g_pnl), sec, 0)) break; }
    put(g_pnl, "Images", "Enable", "1"); puti(g_pnl, "Images", "Count", geti(g_pnl, "Images", "Count", 0) + 1);
    put(g_pnl, sec, "Enable", "1"); put(g_pnl, sec, "Path", ""); put(g_pnl, sec, "X", "0"); put(g_pnl, sec, "Y", "0");
    put(g_pnl, sec, "Width", "0"); put(g_pnl, sec, "Height", "0"); put(g_pnl, sec, "Alpha", "1."); put(g_pnl, sec, "Scale", "1");
}
/* a select bound to a key of the panel */
static void pnl_select(const char *sec, const char *k, const char *label, const opt *o, int n, const char *d, float w) {
    row_begin(label, k, w); row_push(0, w, U(30)); char nv[64];
    if (combo(get(g_pnl, sec, k, d), o, n, w, nv, sizeof nv)) put(g_pnl, sec, k, nv);
    row_end();
}
static int g_item_open = -1;    /* item shown expanded */
static void pnl_control(const char *item, int ctype) {
    char cs[160]; snprintf(cs, sizeof cs, "%sControl", item);
    if (!sec_find(ini(g_pnl), cs, 0)) {   /* create it with defaults */
        put(g_pnl, cs, "Enable", "1"); put(g_pnl, cs, "X", "100"); put(g_pnl, cs, "Y", "100"); put(g_pnl, cs, "CutOff", "1"); put(g_pnl, cs, "Scale", "1");
        if (ctype == 1) { put(g_pnl, cs, "Offset", "0"); put(g_pnl, cs, "Length", "60"); put(g_pnl, cs, "AngleStart", "180"); put(g_pnl, cs, "AngleEnd", "270"); put(g_pnl, cs, "Clockwise", "1"); put(g_pnl, cs, "Color", "0x00ff0000"); }
        else { put(g_pnl, cs, "Mode", "2"); put(g_pnl, cs, "AngleStart", "180"); put(g_pnl, cs, "AngleEnd", "270"); put(g_pnl, cs, "Clockwise", "1");
               put(g_pnl, cs, "WeightLeft", "0."); put(g_pnl, cs, "WeightTop", "0."); put(g_pnl, cs, "WeightRight", "0."); put(g_pnl, cs, "WeightBottom", "0.");
               char im[180]; snprintf(im, sizeof im, "%sImage0", cs); put(g_pnl, im, "Path", ""); put(g_pnl, im, "X", "100"); put(g_pnl, im, "Y", "100"); put(g_pnl, im, "Width", "0"); put(g_pnl, im, "Height", "0"); }
    }
    nk_layout_row_dynamic(ctx, U(30), 1);
    { struct nk_rect r; if (nk_widget(&r, ctx)) draw_text(nk_window_get_canvas(ctx), F_BOLD, r.x + U(16), r.y + U(8), r.w, ctype == 1 ? L("Controle: linha giratória") : L("Controle: imagem animada"), C_ACCENT2); }
    g_rowfirst = 1;
    static const opt modes[] = { { "0", "Cortar (truncar)" }, { "1", "Escalar" }, { "2", "Girar" }, { "3", "Mover entre duas posições" }, { "4", "Alternar imagens" }, { "5", "Preenchimento radial" } };
    static const opt cut[] = { { "0", "Não" }, { "1", "Parar no mínimo/máximo" }, { "2", "Esconder fora do mínimo/máximo" } };
    isec *x = sec_find(ini(g_pnl), cs, 0);
    for (ikey *e = x ? x->keys : NULL; e; e = e->next) {
        if (!strcasecmp(e->k, "Mode") && ctype == 2) { pnl_select(cs, "Mode", L("Modo"), modes, 6, "2", U(260)); continue; }
        if (!strcasecmp(e->k, "CutOff")) { pnl_select(cs, "CutOff", L("Limite no mínimo/máximo"), cut, ctype == 2 ? 3 : 2, "1", U(260)); continue; }
        pnl_value(cs, e->k, e->v);
    }
    if (ctype == 2) for (int n = 0; n < 4; n++) {
        char im[180]; snprintf(im, sizeof im, "%sImage%d", cs, n); isec *y = sec_find(ini(g_pnl), im, 0); if (!y) continue;
        nk_layout_row_dynamic(ctx, U(28), 1);
        { struct nk_rect r; char t[64]; snprintf(t, sizeof t, "%s %d", n ? L("Imagem alternativa") : L("Imagem do controle"), n); if (nk_widget(&r, ctx)) draw_text(nk_window_get_canvas(ctx), F_BOLD, r.x + U(16), r.y + U(7), r.w, n ? L("Imagem alternativa") : L("Imagem do controle"), C_MUTED); (void)t; }
        g_rowfirst = 1;
        for (ikey *e = y->keys; e; e = e->next) pnl_value(im, e->k, e->v);
    }
}
static void pnl_item(int i) {
    char sec[128]; snprintf(sec, sizeof sec, "%s", item_at(i));
    int type = geti(g_pnl, sec, "Type", 0); if (type < 0 || type >= NITEMT) type = 0;
    const char *nm = get(g_pnl, sec, "Name", "");
    char title[300]; snprintf(title, sizeof title, "%d. %s%s%s  ·  [%s]", i + 1, L(ITEM_TYPES[type][1]), *nm ? " — " : "", nm, sec);
    group_begin(title, NULL);
    int open = g_item_open == i;
    row_begin("", NULL, U(560));
    int on = atoi(get(g_pnl, sec, "Enable", "1")) != 0;
    row_push(0, U(42), U(30)); { int n = ui_switch(on); if (n != on) puti(g_pnl, sec, "Enable", n); }
    row_push(U(52), U(140), U(30)); if (nk_button_label(ctx, open ? L("Fechar") : L("Editar"))) { g_item_open = open ? -1 : i; open = !open; }
    row_push(U(198), U(80), U(30)); if (nk_button_label(ctx, L("Subir"))) { pnl_move_item(i, -1); if (open) g_item_open = i - 1; }
    row_push(U(284), U(80), U(30)); if (nk_button_label(ctx, L("Descer"))) { pnl_move_item(i, 1); if (open) g_item_open = i + 1; }
    int del = !strcmp(g_pnl_del, sec);
    row_push(U(370), U(190), U(30)); if (nk_button_label(ctx, del ? L("Confirmar remoção") : L("Remover"))) { if (del) { g_pnl_del[0] = 0; row_end(); group_end(); pnl_del_item(i); g_item_open = -1; return; } snprintf(g_pnl_del, sizeof g_pnl_del, "%s", sec); }
    row_end();
    if (open) {
        static opt types[NITEMT]; for (int t = 0; t < NITEMT; t++) { static char v[NITEMT][4]; snprintf(v[t], 4, "%d", t); types[t].v = v[t]; types[t].label = ITEM_TYPES[t][1]; }
        pnl_select(sec, "Type", L("Tipo"), types, NITEMT, "0", U(300));
        static const opt al[] = { { "0", "Automático" }, { "1", "Esquerda" }, { "2", "Centro" }, { "3", "Direita" } };
        static const opt cm[] = { { "0", "Padrão" }, { "1", "Fixa" }, { "2", "Misturar entre as duas cores" } };
        static const opt ct[] = { { "0", "Nenhum (só texto)" }, { "1", "Linha giratória (ponteiro)" }, { "2", "Imagem animada" } };
        isec *x = sec_find(ini(g_pnl), sec, 0);
        for (ikey *e = x ? x->keys : NULL; e; e = e->next) {
            if (!strcasecmp(e->k, "Type") || !strcasecmp(e->k, "Enable")) continue;
            if (!strcasecmp(e->k, "Align")) { pnl_select(sec, "Align", L("Alinhamento"), al, 4, "0", U(200)); continue; }
            if (!strcasecmp(e->k, "ColorFixed")) { pnl_select(sec, "ColorFixed", L("Modo da cor"), cm, 3, "0", U(260)); continue; }
            if (!strcasecmp(e->k, "ControlType")) { pnl_select(sec, "ControlType", L("Controle"), ct, 3, "0", U(260)); continue; }
            pnl_value(sec, e->k, e->v);
        }
        static const char *add_keys[] = { "Font", "TextColor", "TextAlpha", "TextColor2", "TextAlpha2", "Min", "Max", "TopFreezeDuration", "ShowForTrack", NULL };
        static opt ak[16]; int nk = 0; for (int j = 0; add_keys[j]; j++) if (!x || !key_find(x, add_keys[j])) { ak[nk].v = add_keys[j]; ak[nk].label = add_keys[j]; nk++; }
        if (nk) { row_begin("Mais opções", NULL, U(220)); row_push(0, U(220), U(30)); char nv[64];
            if (combo(L("Adicionar campo…"), ak, nk, U(220), nv, sizeof nv)) put(g_pnl, sec, nv, ci_strstr(nv, "Color") ? "0x00ffffff" : ci_strstr(nv, "Alpha") ? "1." : !strcasecmp(nv, "Font") ? get(g_pnl, "Misc", "Font", "courier") : "0");
            row_end(); }
        int ctype = geti(g_pnl, sec, "ControlType", 0);
        if (ctype == 1 || ctype == 2) pnl_control(sec, ctype);
    }
    group_end();
}
static void pnl_editor(void) {
    const char *F = g_pnl; ifile *fi = ini(F);
    char title[PATH_MAX + 64]; snprintf(title, sizeof title, "%s", Lf("Editando o painel {0}", F + 4, 0, 0));
    group_begin(title, "As alterações são salvas na hora. Posições em pixels numa tela de 1024x768 (ou na resolução indicada em Direita/Base). No jogo: tecla I mostra/esconde, Ctrl+1 a Ctrl+0 ligam e desligam cada painel.");
    row_begin("", NULL, U(560));
    row_push(0, U(150), U(30)); if (nk_button_label(ctx, L("Fechar o editor"))) { g_pnl[0] = 0; g_item_open = -1; row_end(); group_end(); return; }
    { static opt it[NITEMT]; for (int t = 0; t < NITEMT; t++) { static char v[NITEMT][4]; snprintf(v[t], 4, "%d", t); it[t].v = v[t]; it[t].label = ITEM_TYPES[t][1]; }
      char nv[8]; row_push(U(156), U(240), U(30)); if (combo(L("Adicionar item…"), it, NITEMT, U(300), nv, sizeof nv)) pnl_add_item(atoi(nv)); }
    row_push(U(402), U(158), U(30)); if (nk_button_label(ctx, L("Adicionar imagem"))) pnl_add_image();
    row_end();
    group_end();
    /* panel settings */
    group_begin("Geral do painel", NULL);
    static const opt sc[] = { { "0", "Mover para as bordas" }, { "1", "Esticar" }, { "2", "Centralizar" } };
    for (ikey *e = (sec_find(fi, "Misc", 0) ? sec_find(fi, "Misc", 0)->keys : NULL); e; e = e->next) {
        if (!strcasecmp(e->k, "Version")) continue;
        if (!strcasecmp(e->k, "Scaling")) { pnl_select("Misc", "Scaling", L("Ajuste à resolução"), sc, 3, "0", U(240)); continue; }
        pnl_value("Misc", e->k, e->v);
    }
    group_end();
    /* items */
    int c = item_count();
    if (!c) notice(L("Este painel ainda não tem itens. Use “Adicionar item…”."));
    for (int i = 0; i < c && i < 100; i++) pnl_item(i);
    /* static images, filter, ranges and anything else */
    for (isec *x = fi->secs; x; x = x->next) {
        const char *n = x->name; if (!x->keys) continue;
        if (!strcasecmp(n, "Misc") || !strcasecmp(n, "Items")) continue;
        int is_item = 0; for (int i = 0; i < c; i++) { const char *it = item_at(i); size_t l = strlen(it); if (!strcasecmp(n, it) || (!strncasecmp(n, it, l) && !strncasecmp(n + l, "Control", 7))) is_item = 1; }
        if (is_item) continue;
        char sname[160]; snprintf(sname, sizeof sname, "%s", n);
        char gt[300]; snprintf(gt, sizeof gt, "%s  ·  [%s]", ctrl_name(sname), sname);
        group_begin(gt, NULL);
        int im = !strncasecmp(sname, "Image", 5) && isdigit((unsigned char)sname[5]);
        if (im) { row_begin("", NULL, U(190)); int del = !strcmp(g_pnl_del, sname); row_push(0, U(190), U(30));
            if (nk_button_label(ctx, del ? L("Confirmar remoção") : L("Remover imagem"))) { if (del) { g_pnl_del[0] = 0; row_end(); group_end(); sec_delete(sname); puti(g_pnl, "Images", "Count", geti(g_pnl, "Images", "Count", 1) - 1); break; } snprintf(g_pnl_del, sizeof g_pnl_del, "%s", sname); }
            row_end(); }
        for (ikey *e = x->keys; e; e = e->next) pnl_value(sname, e->k, e->v);
        group_end();
    }
}
static void tab_cinfo(void) {
    const char *F = "gpxcinfo.ini";
    if (g_dry) return;
    if (g_pnl[0]) { pnl_editor(); return; }
    group_begin("Painéis", "Os painéis ficam em gpxpdata/pnl. No jogo, a tecla I mostra e esconde os painéis.");
    int used = 0, maxn = 0;
    for (int i = 1; i < 64; i++) { char k[16]; snprintf(k, sizeof k, "Path%d", i); const char *p = get(F, "Panel", k, NULL);
        if (!*p) continue;
        char e[16]; snprintf(e, sizeof e, "Enable%d", i); used++; maxn = i;
        char nm[PATH_MAX]; snprintf(nm, sizeof nm, "pnl\\%s", p); char hp[PATH_MAX]; int ex = lx_file_path(nm, hp, sizeof hp) && !access(hp, F_OK);
        row_begin(p, ex ? NULL : L("Arquivo não encontrado em gpxpdata/pnl."), U(320));
        row_push(0, U(110), U(30)); if (ex && nk_button_label(ctx, L("Editar"))) { snprintf(g_pnl, sizeof g_pnl, "%s", nm); g_scroll_reset_req(); }
        row_push(U(116), U(110), U(30)); if (nk_button_label(ctx, L("Remover"))) { put(F, "Panel", k, NULL); put(F, "Panel", e, NULL); }
        row_push(U(270), U(42), U(30)); { int on = atoi(get(F, "Panel", e, "1")) != 0, n = ui_switch(on); if (n != on) puti(F, "Panel", e, n); }
        row_end();
    }
    if (!used) { nk_layout_row_dynamic(ctx, U(30), 1); nk_label_colored(ctx, L("Nenhum painel configurado."), NK_TEXT_CENTERED, C_MUTED); }
    row_begin("Adicionar painel", "Um painel que já existe em gpxpdata/pnl, ou um novo.", U(470));
    strv l = { 0 };
    for (int i = 0; i < S.panels.n; i++) { const char *a = S.panels.v[i]; if (ci_strstr(a, "fonts.ini")) continue;
        int dup = 0; for (int j = 1; j <= maxn; j++) { char k[16]; snprintf(k, sizeof k, "Path%d", j); if (!strcasecmp(get(F, "Panel", k, ""), a)) dup = 1; }
        if (!dup) sv_add(&l, a); }
    int n; opt *o = opts_from(&l, NULL, NULL, &n); char nv[PATH_MAX];
    row_push(0, U(330), U(30));
    if (combo(L("Adicionar painel…"), o, n, U(330), nv, sizeof nv)) { int k = 1; char kk[16]; for (;; k++) { snprintf(kk, sizeof kk, "Path%d", k); if (!*get(F, "Panel", kk, "")) break; }
        put(F, "Panel", kk, nv); snprintf(kk, sizeof kk, "Enable%d", k); put(F, "Panel", kk, "1"); }
    row_push(U(336), U(134), U(30)); if (nk_button_label(ctx, L("Novo painel…"))) ask(L("Nome do novo painel (ex.: MeuPainel):"), pnl_new_done, NULL);
    sv_clear(&l); free(l.v); row_end();
    group_end();
    group_begin("Exportar", NULL);
    W_check(F, "Export", "Enable", 0, "Exportar os dados do carro", NULL);
    group_end();
}

/* keys */
static const char *KEYLABEL[][2] = {
 {"PanelSize","Painel: aumentar"},{"PanelSizeLess","Painel: diminuir"},{"PanelUp","Painel: subir"},{"PanelDown","Painel: descer"},{"PanelPageUp","Painel: página acima"},{"PanelPageDown","Painel: página abaixo"},
 {"PanelHome","Painel: início"},{"PanelEnd","Painel: fim"},{"PanelLock","Painel: travar"},{"PanelAlphaLess","Painel: mais transparente"},{"PanelAlphaMore","Painel: menos transparente"},{"Screenshot","Captura de tela"},
 {"SAT_ToggleOnOff","TV: mostrar/esconder tudo"},{"SAT_ToggleSpeedSplit","TV: parciais / velocidade"},{"SAT_Standings","TV: classificação"},{"SAT_TowerStandings","TV: torre embaixo"},{"SAT_TowerLeft","TV: torre da esquerda"},
 {"SAT_ScrollNext","TV: próxima página"},{"SAT_TimeLeft","TV: tempo restante / voltas"},{"SAT_MicroTime","TV: microtime"},{"SAT_MicroTimeCompare","TV: microtime, comparação"},{"SAT_MicroTimeSwitchABS","TV: microtime, relativo/absoluto"},
 {"SAT_DirectorsCut","TV: director's cut"},{"SAT_InfoScreen","TV: mapa de informações"},{"SAT_TrackMap","TV: mapa da pista"},
 {"LAP_PageClear","Lap: limpar página"},{"LAP_PageLog","Lap: log"},{"LAP_PageATL","Lap: tabela de tempos"},{"LAP_PageMode","Lap: modo da página"},{"LAP_PageMode2","Lap: modo da página 2"},{"LAP_HotseatJump","Lap: hotseat"},
 {"LAP_SaveCarState","Lap: salvar estado do carro"},{"LAP_RestoreCarState","Lap: restaurar estado do carro"},{"LAP_Quicksave","Lap: quicksave"},
 {"LAP_ToggleSetupMenuOnOff","Lap: menu de acerto"},{"LAP_TogglePitSetupMenuOnOff","Lap: menu de acerto no box"},{"LAP_PitSetupMenuSelect","Lap: acerto no box, escolher"},{"LAP_PitSetupMenuDown","Lap: acerto no box, descer"},{"LAP_PitSetupMenuUp","Lap: acerto no box, subir"},
 {"CInfo_ToggleOnOff","CInfo: mostrar/esconder"},
 {"Game_Replay","Jogo: replay"},{"Game_CamTrack","Jogo: câmera da pista"},{"Game_CamCockpit","Jogo: cockpit"},{"Game_CamChaseFront","Jogo: câmera de perseguição (frente)"},{"Game_CamChaseRear","Jogo: câmera de perseguição (trás)"},
 {"Game_CamOnboard","Jogo: câmera de bordo"},{"Game_CamChaseZoomIn","Jogo: zoom +"},{"Game_CamChaseZoomOut","Jogo: zoom −"},{"Game_CamChaseZoomReset","Jogo: zoom normal"},{"Game_CamChaseShiftUp","Jogo: câmera para cima"},
 {"Game_CamChaseShiftDown","Jogo: câmera para baixo"},{"Game_CamChaseShiftReset","Jogo: câmera normal"},{"Game_DriverName","Jogo: nome do piloto"},{"Game_Gap","Jogo: diferença"},{"Game_QuitRun","Jogo: sair"},
 {"Game_Pit","Jogo: box"},{"Game_ProcessorOccupancy","Jogo: PO"},{"Game_DirectorMode","Jogo: modo diretor (TV)"},{"Game_CarSelectOwn","Jogo: seu carro"},{"Game_CarSelectNext","Jogo: próximo carro"},{"Game_CarSelectPrevious","Jogo: carro anterior"},
 {"Game_HelpAutoBrake","Ajuda: freio automático"},{"Game_HelpAutoGear","Ajuda: câmbio automático"},{"Game_HelpAutoDirection","Ajuda: direção automática"},{"Game_HelpIndestructibility","Ajuda: indestrutível"},
 {"Game_HelpSuggestedLine","Ajuda: linha ideal"},{"Game_HelpSuggestedGear","Ajuda: marcha sugerida"},{"Game_HelpTractionControl","Ajuda: controle de tração"},{"Game_HelpSteeringHelp","Ajuda: direção assistida"},{"Game_HelpLaunchControl","Ajuda: largada"},
 {"Game_ControlSteerRight","Controle: direita"},{"Game_ControlSteerLeft","Controle: esquerda"},{"Game_ControlBrake","Controle: freio"},{"Game_ControlAccelerate","Controle: acelerador"},{"Game_ControlShiftUp","Controle: subir marcha"},{"Game_ControlShiftDown","Controle: descer marcha"},
};
static char g_capture[128];   /* key being captured */
const char *gpx_key_name(SDL_Scancode sc) {
    static char b[16];
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) { b[0] = (char)('A' + sc - SDL_SCANCODE_A); b[1] = 0; return b; }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) { b[0] = (char)('1' + sc - SDL_SCANCODE_1); b[1] = 0; return b; }
    if (sc == SDL_SCANCODE_0) return "0";
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) { snprintf(b, sizeof b, "F%d", 1 + sc - SDL_SCANCODE_F1); return b; }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) { snprintf(b, sizeof b, "NP_%d", 1 + sc - SDL_SCANCODE_KP_1); return b; }
    switch (sc) {
    case SDL_SCANCODE_KP_0: return "NP_0"; case SDL_SCANCODE_KP_DIVIDE: return "NP_/"; case SDL_SCANCODE_KP_MULTIPLY: return "NP_*"; case SDL_SCANCODE_KP_MINUS: return "NP_-";
    case SDL_SCANCODE_KP_PLUS: return "NP_+"; case SDL_SCANCODE_KP_PERIOD: return "NP_."; case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_RETURN: return "ENTER";
    case SDL_SCANCODE_TAB: return "TAB"; case SDL_SCANCODE_INSERT: return "INSERT"; case SDL_SCANCODE_DELETE: return "DELETE"; case SDL_SCANCODE_BACKSPACE: return "BACKSPACE";
    case SDL_SCANCODE_PAGEUP: return "PAGEUP"; case SDL_SCANCODE_PAGEDOWN: return "PAGEDOWN"; case SDL_SCANCODE_HOME: return "HOME"; case SDL_SCANCODE_END: return "END";
    case SDL_SCANCODE_UP: return "UP"; case SDL_SCANCODE_DOWN: return "DOWN"; case SDL_SCANCODE_LEFT: return "LEFT"; case SDL_SCANCODE_RIGHT: return "RIGHT";
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return "SHIFT"; case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return "CTRL";
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return "ALT"; case SDL_SCANCODE_SPACE: return "SPACE"; case SDL_SCANCODE_GRAVE: return "`";
    case SDL_SCANCODE_MINUS: return "-"; case SDL_SCANCODE_EQUALS: return "="; case SDL_SCANCODE_LEFTBRACKET: return "["; case SDL_SCANCODE_RIGHTBRACKET: return "]";
    case SDL_SCANCODE_BACKSLASH: case SDL_SCANCODE_NONUSBACKSLASH: return "\\"; case SDL_SCANCODE_SEMICOLON: return ";"; case SDL_SCANCODE_APOSTROPHE: return "'";
    case SDL_SCANCODE_COMMA: return ","; case SDL_SCANCODE_PERIOD: return "."; case SDL_SCANCODE_SLASH: case SDL_SCANCODE_INTERNATIONAL1: return "/";
    case SDL_SCANCODE_CAPSLOCK: return "CAPSLOCK"; case SDL_SCANCODE_NUMLOCKCLEAR: return "NUMLOCK"; case SDL_SCANCODE_SCROLLLOCK: return "SCROLLLOCK"; case SDL_SCANCODE_PAUSE: return "PAUSE";
    default: return NULL;
    }
}
static void tab_keys(void) {
    if (g_dry) return;
    const char *F = "keys.ini";
    isec *x = sec_find(ini(F), "Binds", 0);
    if (!x || !x->keys) { notice(L("gpxpdata/keys.ini ainda não existe: ele é criado na primeira vez que o jogo roda com o GPxPatch. Inicie o jogo uma vez e volte aqui.")); return; }
    group_begin("Teclas de atalho (gpxpdata/keys.ini)", "Clique na tecla e aperte a nova. Em amarelo: tecla usada por mais de uma ação. As teclas “Jogo:” e “Controle:” também mudam as teclas do próprio GP3.");
    for (ikey *e = x->keys; e; e = e->next) {
        const char *lab = e->k; for (size_t i = 0; i < sizeof KEYLABEL / sizeof *KEYLABEL; i++) if (!strcmp(KEYLABEL[i][0], e->k)) lab = L(KEYLABEL[i][1]);
        int dup = 0; if (strcasecmp(e->v, "TAB") && strcasecmp(e->v, "BACKSPACE")) for (ikey *o = x->keys; o; o = o->next) if (o != e && !strcasecmp(o->v, e->v)) dup = 1;
        row_begin(lab, e->k, U(130)); row_push(0, U(130), U(30));
        int cap = !strcmp(g_capture, e->k);
        if (cap) { nk_style_push_style_item(ctx, &ctx->style.button.normal, nk_style_item_color(C_ACCENT)); nk_style_push_color(ctx, &ctx->style.button.text_normal, nk_rgb(255, 255, 255)); }
        else if (dup) { nk_style_push_style_item(ctx, &ctx->style.button.normal, nk_style_item_color(C_PANEL2)); nk_style_push_color(ctx, &ctx->style.button.text_normal, C_WARN); }
        if (nk_button_label(ctx, cap ? L("aperte…") : e->v)) snprintf(g_capture, sizeof g_capture, "%s", cap ? "" : e->k);
        if (cap || dup) { nk_style_pop_color(ctx); nk_style_pop_style_item(ctx); }
        row_end();
    }
    group_end();
}
static void key_captured(SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_ESCAPE) { g_capture[0] = 0; return; }
    const char *n = gpx_key_name(sc);
    if (!n) { status(L("Essa tecla não pode ser usada pelo GPx.")); g_capture[0] = 0; return; }
    put("keys.ini", "Binds", g_capture, n); g_capture[0] = 0;
}

/* fonts.ini */
static void tab_fonts(void) {
    if (g_dry) return;
    const char *F = "fonts.ini";
    group_begin("Fontes do GPxPatch (gpxpdata/fonts.ini)", NULL);
    ifile *fi = ini(F); int any = 0;
    strv tt = { 0 }; for (int i = 0; i < S.ttfs.n; i++) { char b[PATH_MAX]; snprintf(b, sizeof b, "gfx\\%s", S.ttfs.v[i]); sv_add(&tt, b); }
    for (isec *x = fi->secs; x; x = x->next) {
        any = 1; char s[128]; snprintf(s, sizeof s, "%s", x->name);
        nk_layout_row_dynamic(ctx, U(30), 1);
        { struct nk_rect r; if (nk_widget(&r, ctx)) draw_text(nk_window_get_canvas(ctx), F_BOLD, r.x + U(16), r.y + U(8), r.w, s, C_ACCENT2); }
        g_rowfirst = 1;
        W_text(F, s, "FaceName", "", "Nome da fonte", "Ex.: Courier New, Arial. No Linux é trocada pela equivalente (Liberation).");
        int n; opt *o = opts_from(&tt, "", "(fonte do sistema)", &n); W_sel(F, s, "FontFile", "", "Arquivo da fonte", o, n, NULL, U(260));
        W_num(F, s, "Height", 12, "Altura", "Em 1024x768.", 4, 96, 1, NULL, 0);
        static const opt bo[] = { { "0", "Normal" }, { "400", "Regular" }, { "700", "Negrito" } };
        W_sel(F, s, "Bold", "0", "Peso", bo, 3, NULL, U(200));
        W_check(F, s, "Italic", 0, "Itálico", NULL);
        W_num(F, s, "InterLineSpacing", 0, "Espaço entre linhas", NULL, -20, 40, 1, NULL, 0);
    }
    sv_clear(&tt); free(tt.v);
    if (!any) { nk_layout_row_dynamic(ctx, U(30), 1); nk_label_colored(ctx, L("Nenhuma fonte definida (o GPx usa as padrão)."), NK_TEXT_CENTERED, C_MUTED); }
    row_begin("Nova fonte", "Depois escolha-a nas abas GPxLap e GPxSAT.", U(330));
    static char nf[64]; row_push(0, U(220), U(30)); nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, nf, sizeof nf - 1, nk_filter_default);
    row_push(U(226), U(104), U(30));
    if (nk_button_label(ctx, L("Adicionar")) && nf[0]) { char n2[64]; clean_name(nf, n2, sizeof n2); if (n2[0]) { put(F, n2, "FaceName", "Arial"); put(F, n2, "Height", "14"); } nf[0] = 0; }
    row_end();
    group_end();
}

/* gp3rt.ini */
static void tab_gp3rt(void) {
    if (g_dry) return;
    const char *R = "gp3rt.ini";
    group_begin("Vídeo", NULL);
    static const opt md[] = { { "fullscreen", "Tela cheia" }, { "window", "Janela" } };
    W_sel(R, "video", "mode", "fullscreen", "Tela", md, 2, "Alt+Enter alterna durante o jogo.", U(200));
    static const opt ws[] = { { "1", "640x480" }, { "2", "1280x960" }, { "3", "1920x1440" } };
    W_sel(R, "video", "window_scale", "1", "Tamanho da janela", ws, 3, NULL, U(200));
    static const opt gp[] = { { "auto", "Automático (dedicada se houver duas)" }, { "dedicated", "Sempre a dedicada" }, { "integrated", "Sempre a integrada" }, { "system", "Não mexer" } };
#ifndef _WIN32
    W_sel(R, "video", "gpu", "auto", "Placa de vídeo", gp, 4, NULL, U(300));
#else
    (void)gp;
#endif
    static const opt is[] = { { "auto", "Automático" }, { "1", "1x (original)" }, { "2", "2x" }, { "3", "3x" }, { "4", "4x" } };
    W_sel(R, "video", "internal_scale", "auto", "Resolução interna do 3D", is, 5, NULL, U(200));
    static const opt an[] = { { "auto", "Automático" }, { "0", "Desligada" }, { "2", "2x" }, { "4", "4x" }, { "8", "8x" }, { "16", "16x" } };
    W_sel(R, "video", "anisotropy", "auto", "Filtragem anisotrópica", an, 6, NULL, U(200));
    W_check(R, "video", "force_filtering", 1, "Filtragem bilinear nas texturas", NULL);
    W_check(R, "video", "vsync", 1, "Sincronizar com o monitor (vsync)", NULL);
    W_num(R, "video", "spray_opacity", 60, "Opacidade do spray e da fumaça", "100 = original do GP3.", 0, 100, 1, "%", 0);
    group_end();
    group_begin("Widescreen", NULL);
    W_num(R, "video", "wide_width", -1, "Largura", "-1 = do monitor; 0 = desligado. Escolha 1280x1024 no menu do jogo.", -1, 7680, 1, NULL, 0);
    W_num(R, "video", "wide_height", -1, "Altura", NULL, -1, 4320, 1, NULL, 0);
    W_check(R, "video", "hor_plus", 1, "Campo de visão Hor+", "Mais imagem dos lados, sem esticar.");
    W_num(R, "video", "fov_percent", 100, "Ajuste do campo de visão", NULL, 50, 200, 1, "%", 0);
    group_end();
#ifndef _WIN32
    group_begin("Jogo", NULL);
    static const opt np[] = { { "0", "Segue o NumLock (como no Windows)" }, { "1", "Sempre Home/End/PgUp/PgDn" }, { "2", "Sempre números" } };
    W_sel(R, "game", "numpad_keys", "0", "Teclado numérico", np, 3, NULL, U(300));
    W_check(R, "game", "sound3d", 1, "Som 3D (DirectSound3D)", "Usado pelo GP3 2000 e pelo GPxSound.");
    group_end();
    group_begin("Replay", NULL);
    W_check(R, "game", "replay_keys", 1, "Teclas do replay do GP3 2000 (GP3 1.13)", "F1 a F9 no replay: voltas, pausa, câmera lenta, voltar e avançar. Shift+F1: largada; Shift+F2: fim.");
    W_check(R, "game", "replay_legend", 1, "Legenda das teclas no replay", "Mostra F1 a F9 embaixo da tela.");
    W_check(R, "game", "replay_fx_fix", 1, "Spray e fumaça no replay (GP3 1.13)", "Não somem mais depois de voltar o replay.");
    W_check(R, "game", "gp3replay", 1, "Replay da corrida inteira (GP3 1.13)", "Carrega o GP3Replay.dll quando ele está na pasta do jogo.");
    W_check(R, "game", "replay_2h", 1, "Replay de 2 horas (GP3 2000)", "Length of replay até 2:00:00.");
    group_end();
#endif
}

/* ------------------------------------------------------------------ */
/* GP3 2000: season sets and car sets                                   */
static const char *TEAMS_2000[] = { "McLaren", "Ferrari", "Jordan", "Jaguar", "Williams", "Benetton", "Prost", "Sauber", "Arrows", "Minardi", "BAR" };
static char g_cs_edit[256];
static const char *season_name(void) { const char *s = get("gpxset.ini", "SeasonManager", "SeasonSet", "season.set"); return *s ? s : "season.set"; }
static const char *carset_name(void) {
    if (g_cs_edit[0]) return g_cs_edit;
    const char *c = get(season_name(), "CarSets", "CarSet", "");
    return *c ? c : S.carsets.n ? S.carsets.v[0] : "";
}
/* team / driver names from the performance file of a car set */
static struct { char path[PATH_MAX]; int loaded; char name[11][96], d1[11][96], d2[11][96]; int nteams; } g_perf;
static void perf_load(const char *cs) {
    const char *path = (cs && *cs && atoi(get(cs, "Performance", "Enable", "0"))) ? get(cs, "Performance", "Path", "") : "";
    if (g_perf.loaded && !strcmp(g_perf.path, path)) return;
    memset(&g_perf, 0, sizeof g_perf); snprintf(g_perf.path, sizeof g_perf.path, "%s", path); g_perf.loaded = 1;
    if (!*path) return;
    char hp[PATH_MAX], l1[PATH_MAX]; snprintf(l1, sizeof l1, "%s", path); lx_utf8_to_latin1(l1);
    if (!lx_game_file(path, hp, sizeof hp) && !lx_game_file(l1, hp, sizeof hp)) return;
    size_t n = 0; char *t = lx_read_file(hp, &n); if (!t) return;
    ifile tmp; memset(&tmp, 0, sizeof tmp);
    if (lx_is_utf8(t, n)) file_parse(&tmp, t); else { char *u = latin1_to_utf8(t, n); file_parse(&tmp, u); free(u); }
    free(t);
    for (isec *x = tmp.secs; x; x = x->next) {
        int i; if (sscanf(x->name, "Team #%d", &i) != 1 || i < 0 || i > 10) continue;
        ikey *e = key_find(x, "Name"); if (e) snprintf(g_perf.name[i], 96, "%s", e->v);
        for (int d = 0; d < 2; d++) { e = key_find(x, d ? "Second Driver" : "First Driver");
            if (e) { const char *c = strchr(e->v, ','); if (c) { char b[96]; snprintf(b, sizeof b, "%s", c + 1); char *c2 = strchr(b, ','); if (c2) *c2 = 0; snprintf(d ? g_perf.d2[i] : g_perf.d1[i], 96, "%s", b); } } }
        g_perf.nteams++;
    }
    file_free(&tmp);
}
static void team_info(int t, char *name, size_t nn, char *sub, size_t sn) {
    char n2[8]; snprintf(n2, sizeof n2, "%02d", t + 1);
    if (g_perf.name[t][0]) { snprintf(name, nn, "%s · %s", Lf("Equipe {0}", n2, 0, 0), g_perf.name[t]);
        snprintf(sub, sn, "%s%s%s", g_perf.d1[t], g_perf.d1[t][0] && g_perf.d2[t][0] ? " / " : "", g_perf.d2[t]); }
    else { snprintf(name, nn, "%s", Lf("Equipe {0}", n2, 0, 0)); snprintf(sub, sn, "%s", Lf("{0} no jogo original", TEAMS_2000[t], 0, 0)); }
}
static void W_jam(const char *f, const char *s, const char *k, const char *label, int root, const char *ext, const char *hint) {
    if (g_dry) return;
    row_begin(label, hint, U(400)); file_box(f, s, k, root, ext, label, 0, U(400), 0); row_end();
}
/* fill slots from the files of a folder, in natural order */
static struct { char f[256], sec[64]; int root; int nslots; char keys[66][16]; } g_fill;
static void fill_done(const char *w, void *ud) {
    (void)ud; char wb[PATH_MAX]; snprintf(wb, sizeof wb, "%s\\", w);
    const char *rel = strip_root(g_fill.root, wb);
    if (!rel) { status(Lf("Escolha uma pasta dentro de {0}.", ROOTS[g_fill.root].where, 0, 0)); return; }
    strv files = { 0 }; for (int i = 0; i < B.files.n; i++) sv_add(&files, B.files.v[i]); sv_sort(&files);
    if (!files.n) { status(Lf("Nenhum arquivo {0} nessa pasta.", ".jam", 0, 0)); free(files.v); return; }
    for (int i = 0; i < g_fill.nslots && i < files.n; i++) { char v[PATH_MAX]; snprintf(v, sizeof v, "%s%s", rel, files.v[i]); put(g_fill.f, g_fill.sec, g_fill.keys[i], v); }
    char a[16], b[16]; snprintf(a, sizeof a, "%d", files.n); snprintf(b, sizeof b, "%d", g_fill.nslots);
    status(files.n == g_fill.nslots ? Lf("{0} arquivos atribuídos, em ordem.", a, 0, 0) : Lf("A pasta tem {0} arquivos para {1} posições: confira a ordem.", a, b, 0));
    sv_clear(&files); free(files.v);
}
/* team rows x columns of jam pickers, with the "fill from a folder" button */
static void jam_grid(const char *cs, const char *sec, int ncols, const char *const *cols, int root, int pitcrew_keys, const char *note) {
    if (g_dry) return;
    float tw = U(200), gap = U(6), cellw = (g_cw - U(32) - tw - gap * ncols) / ncols;
    { struct nk_rect a = space_row(U(44), g_cw, 2);
      nk_layout_space_push(ctx, nk_rect(U(16), U(6), U(230), U(32)));
      if (nk_button_label(ctx, L("Preencher com uma pasta…"))) {
          snprintf(g_fill.f, sizeof g_fill.f, "%s", cs); snprintf(g_fill.sec, sizeof g_fill.sec, "%s", sec); g_fill.root = root; g_fill.nslots = 0;
          for (int t = 0; t < 11; t++) for (int c = 0; c < ncols; c++) { char *k = g_fill.keys[g_fill.nslots++];
              if (pitcrew_keys) snprintf(k, 16, "Jam%02d_%02d", t + 1, c + 1); else snprintf(k, 16, "Jam%02d", t * ncols + c + 1); }
          char st[PATH_MAX]; snprintf(st, sizeof st, "%s/%s", lx_dir(), ROOTS[root].start);
          browse("Pasta", ".jam", st, 1, fill_done, NULL);
      }
      if (note) { struct nk_command_buffer *cv = nk_window_get_canvas(ctx); wrap_text(cv, F_SMALL, a.x + U(256), a.y + U(6), a.w - U(272), L(note), C_MUTED, U(16)); } }
    nk_layout_space_end(ctx);
    { struct nk_rect a = space_row(U(24), g_cw, 1); struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
      for (int c = 0; c < ncols; c++) draw_text(cv, F_SMALL, a.x + U(16) + tw + gap + c * (cellw + gap), a.y + U(4), cellw, L(cols[c]), C_MUTED); }
    nk_layout_space_end(ctx);
    for (int t = 0; t < 11; t++) {
        char nm[200], sub[200]; team_info(t, nm, sizeof nm, sub, sizeof sub);
        struct nk_rect a = space_row(U(44), g_cw, 2 * ncols + 1); struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        nk_stroke_line(cv, a.x + U(16), a.y, a.x + a.w - U(16), a.y, U(1), nk_rgb(0x2a, 0x2e, 0x34));
        draw_text(cv, F_BOLD, a.x + U(16), a.y + U(5), tw - U(6), nm, C_TEXT);
        draw_text(cv, F_SMALL, a.x + U(16), a.y + U(24), tw - U(6), sub, C_MUTED);
        g_row = nk_rect(U(16) + tw + gap, 0, g_cw, U(44));
        for (int c = 0; c < ncols; c++) {
            char k[16]; if (pitcrew_keys) snprintf(k, sizeof k, "Jam%02d_%02d", t + 1, c + 1); else snprintf(k, sizeof k, "Jam%02d", t * ncols + c + 1);
            file_box(cs, sec, k, root, ".jam", cols[c], c * (cellw + gap), cellw, 1);
        }
        nk_layout_space_end(ctx);
    }
}
static void points_grid(const char *f, const char *sec, int n, const int *def, int mn, int mx, const char *fmt) {
    if (g_dry) return;
    int per = (int)((g_cw - U(32)) / U(118)); if (per < 2) per = 2;
    for (int i = 0; i < n; i += per) {
        space_row(U(40), g_cw, 2 * per);
        for (int j = i; j < n && j < i + per; j++) {
            float x = U(16) + (j - i) * U(118);
            char k[16], lab[32]; snprintf(k, sizeof k, "Pos%02d", j + 1); snprintf(lab, sizeof lab, fmt, j + 1);
            nk_layout_space_push(ctx, nk_rect(x, U(5), U(40), U(30))); nk_label_colored(ctx, lab, NK_TEXT_RIGHT, C_MUTED);
            nk_layout_space_push(ctx, nk_rect(x + U(44), U(5), U(68), U(30)));
            char id[64]; snprintf(id, sizeof id, "##%s%d", sec, j);
            int v = geti(f, sec, k, def ? def[j] : j + 1), v0 = v;
            nk_property_int(ctx, id, mn, &v, mx, 1, 1);
            if (v != v0) puti(f, sec, k, v);
        }
        nk_layout_space_end(ctx);
    }
}
/* select one of the .set files + new / duplicate */
static int g_ask_kind;   /* 0 season, 1 car set */
static void set_created(const char *n) {
    if (g_ask_kind == 0) { put("gpxset.ini", "SeasonManager", "SeasonSet", n); }
    else if (g_ask_kind == 2) { char k[16]; put(season_name(), "CarSets", cs_key(k, sizeof k), n); flush(); scan(); open_carset(n); return; }
    else snprintf(g_cs_edit, sizeof g_cs_edit, "%s", n);
    flush(); scan();
}
static void ask_new(const char *t, void *ud) {
    (void)ud; char n[256]; clean_name(t, n, sizeof n); if (!n[0]) return; strcat(n, ".set");
    if (name_taken(n)) { status(L("Já existe um arquivo com esse nome.")); return; }
    if (g_ask_kind == 0) { put(n, "CarSets", "Enable", "1"); put(n, "CarSets", "CarSet", carset_name()); put(n, "CarSets", "EnableTrackSpecific", "0"); }
    else { put(n, "Performance", "Enable", "0"); put(n, "TeamPitOrder", "Enable", "0"); }
    flush(); set_created(n);
}
static char g_dup_from[256];
static void ask_dup(const char *t, void *ud) {
    (void)ud; char n[256]; clean_name(t, n, sizeof n); if (!n[0]) return; strcat(n, ".set");
    if (name_taken(n)) { status(L("Já existe um arquivo com esse nome.")); return; }
    flush(); if (!copy_set(g_dup_from, n)) { status(L("Não foi possível copiar.")); return; }
    set_created(n);
}
static int to_latin1(const char *name) {
    char p[PATH_MAX]; if (!lx_file_path(name, p, sizeof p)) return 0;
    size_t n = 0; char *t = lx_read_file(p, &n); int ok = 0;
    if (t && lx_is_utf8(t, n)) { lx_utf8_to_latin1(t); char tmp[PATH_MAX + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", p);
        FILE *f = fopen(tmp, "wb"); if (f) { fputs(t, f); fclose(f); ok = rename(tmp, p) == 0; } }
    free(t); return ok;
}
static void utf8_notice(const char *n) {
    if (g_dry || !ini(n)->utf8) return;
    W_note(Lf("{0} está gravado em UTF-8. No Linux o GP3RT entende os acentos (ç, ã, é) mesmo assim, mas o GPxPatch do Windows só entende Latin-1 (Windows-1252). ", n, 0, 0), 1);
    space_row(U(36), g_cw, 1); nk_layout_space_push(ctx, nk_rect(0, 0, U(220), U(32)));
    if (nk_button_label(ctx, L("Converter para Latin-1"))) { flush(); if (to_latin1(n)) { ini_forget(n); status(Lf("{0} convertido para Latin-1.", n, 0, 0)); } else status(Lf("Não foi possível converter {0}.", n, 0, 0)); }
    nk_layout_space_end(ctx); space(U(10));
}
static void set_picker(const char *label, const char *hint, const char *cur, const strv *list, int kind) {
    if (g_dry) return;
    row_begin(label, hint, U(520));
    strv l = { 0 }; if (*cur && !sv_has(list, cur)) sv_add(&l, cur); for (int i = 0; i < list->n; i++) sv_add(&l, list->v[i]);
    int n; opt *o = opts_from(&l, NULL, NULL, &n); char nv[256];
    row_push(0, U(300), U(30));
    if (combo(*cur ? cur : L("(nenhum)"), o, n, U(300), nv, sizeof nv)) { if (kind == 0) { put("gpxset.ini", "SeasonManager", "SeasonSet", nv); flush(); } else { snprintf(g_cs_edit, sizeof g_cs_edit, "%s", nv); g_perf.loaded = 0; } }
    row_push(U(306), U(100), U(30)); if (nk_button_label(ctx, L("Novo…"))) { g_ask_kind = kind; ask(kind == 0 ? L("Nome da nova temporada (ex.: F1-1998):") : L("Nome do novo carset (ex.: F1-1998-carros):"), ask_new, NULL); }
    if (*cur) { row_push(U(412), U(108), U(30)); if (nk_button_label(ctx, L("Duplicar…"))) { g_ask_kind = kind; snprintf(g_dup_from, sizeof g_dup_from, "%s", cur); ask(Lf("Nome da cópia de {0}:", cur, 0, 0), ask_dup, NULL); } }
    sv_clear(&l); free(l.v); row_end();
}
/* small tab bar inside a page; returns the selected index */
static int seg_tabs(const char *const *names, int n, int cur) {
    if (g_dry) return cur;
    float x = 0, h = U(34); struct nk_rect a = space_row(h + U(8), g_cw, n + 1); struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    nk_stroke_line(cv, a.x, a.y + h + U(4), a.x + a.w, a.y + h + U(4), U(1), C_LINE);
    for (int i = 0; i < n; i++) {
        float w = text_w(F_BOLD, L(names[i]), -1) + U(32);
        struct nk_rect r = nk_rect(a.x + x, a.y, w, h + U(4));
        int hov = nk_input_is_mouse_hovering_rect(&ctx->input, r);
        if (i == cur) { nk_fill_rect(cv, nk_rect(r.x, r.y, r.w, h + U(4)), U(7), C_PANEL2); nk_fill_rect(cv, nk_rect(r.x, r.y + h + U(1), r.w, U(3)), 0, C_ACCENT); }
        else if (hov) nk_fill_rect(cv, nk_rect(r.x, r.y, r.w, h), U(7), C_PANEL);
        draw_text(cv, i == cur ? F_BOLD : F_TEXT, r.x + U(16), r.y + U(9), w, L(names[i]), i == cur ? C_TEXT : C_MUTED);
        if (nk_input_is_mouse_click_in_rect(&ctx->input, NK_BUTTON_LEFT, r) && !(ctx->current->layout->flags & NK_WINDOW_ROM)) { cur = i; g_scroll_reset_req(); }
        x += w + U(4);
    }
    nk_layout_space_end(ctx); space(U(10));
    return cur;
}
/* "‹ Voltar" + a title */
static int sub_header(const char *back, const char *title) {
    if (g_dry) return 0;
    space_row(U(40), g_cw, 2);
    nk_layout_space_push(ctx, nk_rect(0, U(4), U(150), U(32)));
    char bl[160]; snprintf(bl, sizeof bl, "\xe2\x86\x90  %s", L(back)); int r = nk_button_label(ctx, bl);
    nk_layout_space_push(ctx, nk_rect(U(164), U(4), g_cw - U(164), U(32))); nk_label(ctx, title, NK_TEXT_LEFT);
    nk_layout_space_end(ctx); space(U(8));
    return r;
}
static void season_select_row(void) {
    const char *G = "gpxset.ini"; ensure(G, "SeasonManager", "Enable", "0");
    if (g_dry) return;
    char cur[256]; snprintf(cur, sizeof cur, "%s", season_name());
    row_begin("Selecionar temporada", "Pinturas, capacetes, mecânicos, pneus, pontos e pistas de uma temporada (.set na pasta gpxpdata).", U(560));
    int on = atoi(get(G, "SeasonManager", "Enable", "0")) != 0; row_push(0, U(42), U(30)); int n = ui_switch(on); if (n != on) puti(G, "SeasonManager", "Enable", n);
    strv l = { 0 }; if (!sv_has(&S.seasons, cur)) sv_add(&l, cur); for (int i = 0; i < S.seasons.n; i++) sv_add(&l, S.seasons.v[i]);
    int no; opt *o = opts_from(&l, NULL, NULL, &no); char nv[256];
    row_push(U(54), U(270), U(30)); if (combo(cur, o, no, U(270), nv, sizeof nv)) { put(G, "SeasonManager", "SeasonSet", nv); flush(); }
    sv_clear(&l); free(l.v);
    row_push(U(330), U(110), U(30));
    nk_style_push_style_item(ctx, &ctx->style.button.normal, nk_style_item_color(C_ACCENT));
    nk_style_push_style_item(ctx, &ctx->style.button.hover, nk_style_item_color(C_ACCENT2));
    nk_style_push_color(ctx, &ctx->style.button.text_normal, nk_rgb(255, 255, 255)); nk_style_push_color(ctx, &ctx->style.button.text_hover, nk_rgb(255, 255, 255));
    if (nk_button_label(ctx, L("Editar…"))) { g_sub = 1; g_scroll_reset_req(); }
    nk_style_pop_color(ctx); nk_style_pop_color(ctx); nk_style_pop_style_item(ctx); nk_style_pop_style_item(ctx);
    row_push(U(446), U(114), U(30)); if (nk_button_label(ctx, L("Novo…"))) { g_ask_kind = 0; ask(L("Nome da nova temporada (ex.: F1-1998):"), ask_new, NULL); }
    row_end();
}
static const char *cs_key(char *k, size_t n) { if (g_cs_slot) snprintf(k, n, "CarSet%02d", g_cs_slot); else snprintf(k, n, "CarSet"); return k; }
static void open_carset(const char *n) { snprintf(g_cs_edit, sizeof g_cs_edit, "%s", n); g_perf.loaded = 0; g_sub = 2; g_cs_tab = 0; g_scroll_reset_req(); }
static void season_carset(const char *F) {
    group_begin("Carsets", NULL);
    W_check(F, "CarSets", "Enable", 1, "Usar os carsets desta temporada", "Pinturas e cockpits dos 22 carros, capacetes, mecânicos, performance e ordem dos boxes.");
    W_check(F, "CarSets", "EnableTrackSpecific", 0, "Ativar carsets específicos da pista", "Cada pista pode ter o seu carset; as que não têm usam o de todas as pistas.");
    int ts = atoi(get(F, "CarSets", "EnableTrackSpecific", "0")) != 0; if (!ts) g_cs_slot = 0;
    strv cs = { 0 }; for (int i = 0; i < S.carsets.n; i++) sv_add(&cs, S.carsets.v[i]); for (int i = 0; i < S.seasons.n; i++) sv_add(&cs, S.seasons.v[i]); sv_sort(&cs);
    char key[16]; cs_key(key, sizeof key);
    if (ts) {
        row_begin("Carset para a pista", NULL, U(330));
        static char nm[18][80]; static opt so[18];
        snprintf(nm[0], 80, "%s", L("Todas as pistas")); so[0].v = "0"; so[0].label = nm[0];
        static char vs[18][4]; for (int i = 0; i < 17; i++) { snprintf(vs[i + 1], 4, "%d", i + 1); snprintf(nm[i + 1], 80, "%02d  %s", i + 1, L(SLOTS_2000[i])); so[i + 1].v = vs[i + 1]; so[i + 1].label = nm[i + 1]; }
        char cv[8]; snprintf(cv, sizeof cv, "%d", g_cs_slot); char nv[8];
        row_push(0, U(330), U(30)); if (combo_shown(nm[g_cs_slot], so, 18, U(330), nv, sizeof nv)) g_cs_slot = atoi(nv);
        row_end(); cs_key(key, sizeof key);
    }
    {
        const char *cur = get(F, "CarSets", key, "");
        row_begin(g_cs_slot ? "Carset desta pista" : "Carset de todas as pistas", g_cs_slot ? "Vazio: usa o carset de todas as pistas." : "Arquivo .set com as seções [Cars], [Cockpits]… na pasta gpxpdata.", U(560));
        strv l = { 0 }; if (*cur && !sv_has(&cs, cur)) sv_add(&l, cur); for (int i = 0; i < cs.n; i++) sv_add(&l, cs.v[i]);
        int no; opt *o = opts_from(&l, "", g_cs_slot ? "(o de todas as pistas)" : "(nenhum)", &no); char nv[256];
        row_push(0, U(320), U(30));
        if (combo(cur, o, no, U(320), nv, sizeof nv)) { put(F, "CarSets", key, nv[0] ? nv : NULL); }
        sv_clear(&l); free(l.v);
        row_push(U(326), U(114), U(30));
        if (*cur) { nk_style_push_style_item(ctx, &ctx->style.button.normal, nk_style_item_color(C_ACCENT)); nk_style_push_style_item(ctx, &ctx->style.button.hover, nk_style_item_color(C_ACCENT2));
            nk_style_push_color(ctx, &ctx->style.button.text_normal, nk_rgb(255, 255, 255)); nk_style_push_color(ctx, &ctx->style.button.text_hover, nk_rgb(255, 255, 255));
            if (nk_button_label(ctx, L("Editar…"))) open_carset(cur);
            nk_style_pop_color(ctx); nk_style_pop_color(ctx); nk_style_pop_style_item(ctx); nk_style_pop_style_item(ctx); }
        else nk_spacing(ctx, 1);
        row_push(U(446), U(114), U(30)); if (nk_button_label(ctx, L("Novo…"))) { g_ask_kind = 2; ask(L("Nome do novo carset (ex.: F1-1998-carros):"), ask_new, NULL); }
        row_end();
    }
    if (ts && !g_dry) {   /* which car set each track uses */
        float half = (g_cw - U(32) - U(24)) / 2; int rows = 9;
        for (int r = 0; r < rows; r++) {
            struct nk_rect a = space_row(U(26), g_cw, 3); struct nk_command_buffer *cvv = nk_window_get_canvas(ctx);
            for (int c = 0; c < 2; c++) { int i = c * rows + r; if (i >= 17) continue;
                char k[16]; snprintf(k, sizeof k, "CarSet%02d", i + 1); const char *v = get(F, "CarSets", k, "");
                struct nk_rect cr = nk_rect(a.x + U(16) + c * (half + U(24)), a.y, half, U(26));
                int sel = g_cs_slot == i + 1, hov = nk_input_is_mouse_hovering_rect(&ctx->input, cr);
                if (sel || hov) nk_fill_rect(cvv, cr, U(5), sel ? C_PANEL2 : C_PANEL);
                char t[160]; snprintf(t, sizeof t, "%02d  %s", i + 1, L(SLOTS_2000[i]));
                draw_text(cvv, F_TEXT, cr.x + U(8), cr.y + U(4), half * 0.45f, t, C_MUTED);
                draw_text(cvv, F_TEXT, cr.x + half * 0.45f, cr.y + U(4), half * 0.55f - U(8), *v ? v : L("(o de todas as pistas)"), *v ? C_TEXT : C_MUTED);
                if (nk_input_is_mouse_click_in_rect(&ctx->input, NK_BUTTON_LEFT, cr) && !(ctx->current->layout->flags & NK_WINDOW_ROM)) g_cs_slot = i + 1;
            }
            nk_layout_space_end(ctx);
        }
        space(U(6));
    }
    W_check(F, "CarSets", "DisableCarNrCopying", 0, "Desativar cópia do número do carro", "Use quando cada pintura já tem o número certo pintado.");
    group_end();
    sv_clear(&cs); free(cs.v);
    group_begin("Arquivos da temporada", "Carshape e física da aba GPxSet têm prioridade sobre os da temporada.");
    { int n; opt *o = opts_from(&S.sets, "", "(nenhum)", &n); W_switch_sel(F, "TrackSet", "Enable", 0, "Selecionar pistas", "Um conjunto de pistas da aba GPxTrack. A pista escolhida num carset tem prioridade.", "TrackSet", "Path", "", o, n, U(300)); }
    W_switch_file(F, "Carshape", "Enable", 0, "Carshape", NULL, "Carshape", "Carshape", R_GAME, ".cs3");
    W_switch_file(F, "Physics", "Enable", 0, "Física", NULL, "Physics", "Path", R_GAME, ".g3p");
    W_file_al(F, "Physics", "TorquePath", "Torque", R_GAME, ".ini", NULL);
    W_switch_file(F, "MenuGfx", "Enable", 0, "Gráficos do menu", "Padrão: bitmaps\\gp2000v1.bin.", "MenuGfx", "Path", R_GAME, ".bin");
    W_switch_sel(F, "SATMode", "Enable", 0, "GPxSAT modalidade", "Os gráficos de TV usados nesta temporada.", "SATMode", "Mode", "0", SATMODES, 4, U(200));
    group_end();
}
static void season_tyres(const char *F) {
    perf_load(carset_name());
    group_begin("Pneus", NULL);
    W_check(F, "Tyres", "Enable", 0, "Dois tipos de pneu", "O GP3 2000 original tem um só.");
    W_jam(F, "Tyres", "Jam01_01", "Tipo 1: pneu (lento)", R_MAIN, ".jam", "Em gp3jams\\main; no modo hardware precisa do .bmp de mesmo nome em gp3jamsh\\techjam.");
    W_jam(F, "Tyres", "Jam01_02", "Tipo 1: pneu (rápido, borrado)", R_MAIN, ".jam", NULL);
    W_jam(F, "Tyres", "Jam02_01", "Tipo 2: pneu (lento)", R_MAIN, ".jam", NULL);
    W_jam(F, "Tyres", "Jam02_02", "Tipo 2: pneu (rápido, borrado)", R_MAIN, ".jam", NULL);
    W_jam(F, "Tyres", "TreadArt01_01", "Tipo 1: banda de chuva (lenta)", R_TECH, ".bmp", NULL);
    W_jam(F, "Tyres", "TreadArt01_02", "Tipo 1: banda de chuva (rápida)", R_TECH, ".bmp", NULL);
    W_jam(F, "Tyres", "TreadArt02_01", "Tipo 2: banda de chuva (lenta)", R_TECH, ".bmp", NULL);
    W_jam(F, "Tyres", "TreadArt02_02", "Tipo 2: banda de chuva (rápida)", R_TECH, ".bmp", NULL);
    W_text(F, "Tyres", "Tread", "tread", "Prefixo dos arquivos tread??.bin", "Modo software, em gp3jamsh\\techjam.");
    W_text(F, "Tyres", "SatType01", "", "Tipo 1: marca nos gráficos de TV", NULL);
    W_text(F, "Tyres", "SatType02", "", "Tipo 2: marca nos gráficos de TV", NULL);
    group_end();
    group_begin("Pneu de cada equipe", NULL);
    static const opt ty[] = { { "Jam01", "Tipo 1" }, { "Jam02", "Tipo 2" } };
    for (int t = 0; t < 11; t++) { char k[16], nm[200], sub[200]; snprintf(k, sizeof k, "Team%02d", t + 1); team_info(t, nm, sizeof nm, sub, sizeof sub); W_sel(F, "Tyres", k, "Jam01", nm, ty, 2, sub, U(140)); }
    group_end();
}
static void season_points(const char *F) {
    group_begin("Pontos", NULL);
    W_check(F, "RacePoints", "Enable", 0, "Pontuação da temporada", "Pontos de cada posição na corrida.");
    if (!g_dry) {
        row_begin("Modelos", NULL, U(360));
        static const int P91[22] = { 10, 6, 4, 3, 2, 1 }, P03[22] = { 10, 8, 6, 5, 4, 3, 2, 1 }, P10[22] = { 25, 18, 15, 12, 10, 8, 6, 4, 2, 1 };
        const int *pp = NULL;
        row_push(0, U(116), U(30)); if (nk_button_label(ctx, "1991–2002")) pp = P91;
        row_push(U(122), U(116), U(30)); if (nk_button_label(ctx, "2003–2009")) pp = P03;
        row_push(U(244), U(116), U(30)); if (nk_button_label(ctx, "2010+")) pp = P10;
        row_end();
        if (pp) { for (int i = 0; i < 22; i++) { char k[16]; snprintf(k, sizeof k, "Pos%02d", i + 1); puti(F, "RacePoints", k, pp[i]); } put(F, "RacePoints", "Enable", "1"); }
    }
    { static const int def[22] = { 10, 6, 4, 3, 2, 1 }; points_grid(F, "RacePoints", 22, def, 0, 255, "%dº"); }
    group_end();
}
static void season_page(void) {
    if (g_dry) return;
    const char *G = "gpxset.ini"; char F[256]; snprintf(F, sizeof F, "%s", season_name());
    char t[400]; snprintf(t, sizeof t, "%s", Lf("Temporada: {0}", F, 0, 0));
    if (sub_header("Voltar", t)) { g_sub = 0; g_scroll_reset_req(); return; }
    if (!atoi(get(G, "SeasonManager", "Enable", "0"))) notice(L("Esta temporada ainda não está ligada: o jogo usa os carros originais. Ligue “Selecionar temporada” na aba GPxSet."));
    if (!ini(F)->secs) { notice(Lf("O arquivo {0} não existe ou está vazio. Crie uma temporada com “Novo…”.", F, 0, 0)); return; }
    utf8_notice(F);
    static const char *const T[] = { "Carset", "Pneus", "Pontos" };
    g_sea_tab = seg_tabs(T, 3, g_sea_tab);
    if (g_sea_tab == 0) season_carset(F); else if (g_sea_tab == 1) season_tyres(F); else season_points(F);
    if (!g_dry) { space_row(U(40), g_cw, 2); nk_layout_space_push(ctx, nk_rect(0, U(4), U(200), U(32)));
        if (nk_button_label(ctx, L("Duplicar temporada…"))) { g_ask_kind = 0; snprintf(g_dup_from, sizeof g_dup_from, "%s", F); ask(Lf("Nome da cópia de {0}:", F, 0, 0), ask_dup, NULL); }
        nk_layout_space_end(ctx); }
}
static void carset_page(void) {
    if (g_dry) return;
    char F[256]; snprintf(F, sizeof F, "%s", carset_name());
    char t[400]; snprintf(t, sizeof t, "%s", Lf("Carset: {0}", F, 0, 0));
    if (sub_header("Temporada", t)) { g_sub = 1; g_cs_edit[0] = 0; g_scroll_reset_req(); return; }
    strv l = { 0 }; for (int i = 0; i < S.carsets.n; i++) sv_add(&l, S.carsets.v[i]); for (int i = 0; i < S.seasons.n; i++) sv_add(&l, S.seasons.v[i]); sv_sort(&l);
    group_begin("Carset em edição", NULL);
    set_picker("Arquivo", "Carsets ficam na pasta gpxpdata. A temporada escolhe qual usar em cada pista.", F, &l, 1);
    group_end();
    sv_clear(&l); free(l.v);
    if (!F[0]) { notice(L("Nenhum carset ainda. Crie um com “Novo…”.")); return; }
    if (!ini(F)->secs) notice(Lf("O arquivo {0} ainda está vazio.", F, 0, 0));
    utf8_notice(F);
    perf_load(F);
    static const char *const T[] = { "Equipes e pista", "Pinturas", "Cockpits", "Capacetes", "Mecânicos", "Equipe de box" };
    g_cs_tab = seg_tabs(T, 6, g_cs_tab);
    static const char *const CARS[] = { "Carro 1", "Carro 2" }, *const JM[] = { "Macaco 1", "Macaco 2" }, *const PC[] = { "1", "2", "3", "4", "5", "6" };
    switch (g_cs_tab) {
    case 0:
        group_begin("Equipes e pilotos", NULL);
        W_check(F, "Performance", "Enable", 0, "Usar arquivo de performance", "Nomes, performance, pneus e câmbio das equipes.");
        W_jam(F, "Performance", "Path", "Arquivo de performance", R_GAME, ".txt", "Feito com o GP3 2000 Editor / TeamEditor.");
        if (g_perf.path[0] && !g_perf.nteams && exists_cached(R_GAME, g_perf.path)) notice(Lf("Nenhuma equipe encontrada em {0}.", g_perf.path, 0, 0));
        W_check(F, "TeamPitOrder", "Enable", 0, "Ordem dos boxes", "Equipe em cada posição do pitlane (1 = primeiro box).");
        points_grid(F, "TeamPitOrder", 11, NULL, 1, 11, "Box %d");
        group_end();
        group_begin("Pista", NULL);
        W_check(F, "Track", "Enable", 0, "Pista própria deste carset", "Tem prioridade sobre o conjunto de pistas (a não ser que a aba GPxTrack diga para ignorar). Só vale quando o carset é de uma pista.");
        W_jam(F, "Track", "Path", "Arquivo da pista", R_GAME, ".dat", NULL);
        W_jam(F, "TrackJams", "Podgen04", "Jam do box (podgen04)", R_JAMS, ".jam", "Em gp3jams; padrão: genjam\\podgen04.jam.");
        { int n; opt *o = opts_from(&S.tarmacs, "", "(o da aba GPxSet)", &n); W_sel(F, "TrackJams", "Tarmac", "", "Bitmap do asfalto", o, n, NULL, U(260)); }
        group_end();
        break;
    case 1:
        group_begin("Pinturas dos carros", NULL);
        jam_grid(F, "Cars", 2, CARS, R_LIV, 0, "Os arquivos ficam em gp3jams\\liveries ou gp3jamsh\\liveries (ou subpastas). O GPx ordena as pinturas pelo id do jam: confira se cada uma está na equipe certa.");
        group_end(); break;
    case 2: group_begin("Cockpits", NULL); jam_grid(F, "Cockpits", 2, CARS, R_LIV, 0, NULL); group_end(); break;
    case 3:
        group_begin("Capacetes", NULL);
        W_jam(F, "Helmets", "Jam01", "Capacetes 1", R_LIV, ".jam", "O nome precisa começar com “helmet”, e a versão software (gp3jams) precisa existir.");
        W_jam(F, "Helmets", "Jam02", "Capacetes 2", R_LIV, ".jam", NULL);
        group_end(); break;
    case 4: group_begin("Mecânicos do macaco", NULL); jam_grid(F, "JackMen", 2, JM, R_MAIN, 1, "Em gp3jams\\main ou gp3jamsh\\main (ou subpastas)."); group_end(); break;
    default: group_begin("Equipe de box", NULL); jam_grid(F, "PitCrews", 6, PC, R_MAIN, 1, NULL); group_end(); break;
    }
}
/* credits */
static void para(const char *who, const char *text) {   /* a name in bold + a plain paragraph */
    if (g_dry) return;
    float lw = g_cw - U(32); int n = wrap_text(NULL, F_TEXT, 0, 0, lw, text, C_TEXT, U(20));
    float h = (who ? U(22) : 0) + n * U(20) + U(12);
    struct nk_rect a = space_row(h, g_cw, 1); struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    float y = a.y + U(2);
    if (who) { draw_text(cv, F_BOLD, a.x + U(16), y, lw, who, C_TEXT); y += U(22); }
    wrap_text(cv, F_TEXT, a.x + U(16), y, lw, text, C_MUTED, U(20));
    nk_layout_space_end(ctx);
}
static SDL_Texture *g_logo;
static void tab_about(void) {
    if (g_dry) return;
    if (!g_logo) { extern const unsigned char *gpxp_icon128(void);
        g_logo = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, 128, 128);
        if (g_logo) { SDL_UpdateTexture(g_logo, NULL, gpxp_icon128(), 128 * 4); SDL_SetTextureBlendMode(g_logo, SDL_BLENDMODE_BLEND); } }
    { struct nk_rect a = space_row(U(140), g_cw, 1); struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
      if (g_logo) { struct nk_image im = nk_image_ptr(g_logo); nk_draw_image(cv, nk_rect(a.x, a.y + U(6), U(128), U(128)), &im, nk_rgb(255, 255, 255)); }
#ifdef _WIN32
      draw_text(cv, F_TITLE, a.x + U(150), a.y + U(28), a.w - U(150), "GPxPatch GP3RT", C_TEXT);
      char v[200]; snprintf(v, sizeof v, "%s %s", L("GP3RT · a tela do GPxPatch 4.52 para Windows · versão"), lx_version());
#else
      draw_text(cv, F_TITLE, a.x + U(150), a.y + U(28), a.w - U(150), L("GPxPatch para Linux"), C_TEXT);
      char v[200]; snprintf(v, sizeof v, "%s %s", L("GP3RT · edição para Linux do GPxPatch 4.52 · versão"), lx_version());
#endif
      draw_text(cv, F_TEXT, a.x + U(150), a.y + U(66), a.w - U(150), v, C_MUTED);
#ifdef _WIN32
      draw_text(cv, F_SMALL, a.x + U(150), a.y + U(92), a.w - U(150), L("A mesma tela do GP3RT para Linux, agora também no Windows."), C_MUTED);
#else
      draw_text(cv, F_SMALL, a.x + U(150), a.y + U(92), a.w - U(150), L("Grand Prix 3 e Grand Prix 3 2000 nativos no Linux, sem Wine."), C_MUTED);
#endif
      nk_layout_space_end(ctx); }
    space(U(10));
    group_begin("GPxPatch original (Windows)", NULL);
    para("René Smit", L("Autor do GPxPatch (2001–2019): GPxSet, GPxTrack, GPxLap, GPxCInfo, GPxSound e a tela de configuração original."));
    para("Marc Aarts · Markus Krötz", L("GPxSAT Classic e Digital (Marc Aarts, 2001–2004) e GPxSAT Digital 2004 (Markus Krötz)."));
    para("Laurent Rousseau · Victor Gars", L("Painel Lo2k Blue Theme do GPxCInfo (2004) e os adjetivos dos países dos menus (adjectiv.ini)."));
    para(NULL, L("O GPxPatch é freeware, de René Smit, e não vem neste pacote: baixe o GPxPatch 4.52 no site do autor (www.sdidit.nl) e instale na pasta do jogo, como explica o README / LEIAME."));
    group_end();
#ifdef _WIN32
    group_begin("GP3RT", NULL);
#else
    group_begin("Edição para Linux", NULL);
#endif
    para("Rodrigo Motta", L("GPxPatch para Linux e GP3RT: o carregador que roda o Gp3.exe original no Linux, sem Wine, e esta tela de configuração refeita em C a partir da original, com os mesmos arquivos .ini e .set, para que tudo funcione igual no Windows e no Linux."));
    para("Claude (Anthropic)", L("Desenvolvimento junto com o Rodrigo."));
    group_end();
    group_begin("Jogo e componentes", NULL);
    para("Geoff Crammond · MicroProse", L("Grand Prix 3 (1.13) e Grand Prix 3 2000."));
    para(NULL, L("SDL2 (licença zlib), Nuklear (domínio público / MIT) e a fonte Inter de Rasmus Andersson (SIL Open Font License)."));
    group_end();
}

/* ------------------------------------------------------------------ */
/* the window                                                           */
typedef struct { const char *id, *name; void (*build)(void); int only2000; int sep_before; } tab_t;
static const tab_t TABS[] = {
    { "gpx", "GPxPatch", tab_gpx, 0, 0 }, { "set", "GPxSet", tab_set, 0, 0 },
    { "track", "GPxTrack", tab_track, 0, 0 }, { "lap", "GPxLap", tab_lap, 0, 0 }, { "sat", "GPxSAT", tab_sat, 0, 0 }, { "cinfo", "GPxCInfo", tab_cinfo, 0, 0 },
    { "sound", "GPxSound", tab_sound, 0, 0 }, { "keys", "Teclas", tab_keys, 0, 1 }, { "fonts", "Fontes", tab_fonts, 0, 0 }, { "gp3rt", "Vídeo e jogo (GP3RT)", tab_gp3rt, 0, 1 },
    { "about", "Créditos", tab_about, 0, 0 },
};
#define NTABS ((int)(sizeof TABS / sizeof *TABS))
static int g_tab; static int g_scroll_reset;
static void g_scroll_reset_req(void) { g_scroll_reset = 1; }
void ui_goto(const char *id) { for (int i = 0; i < NTABS; i++) if (!strcmp(TABS[i].id, id)) { g_tab = i; g_scroll_reset = 1; } }
static int g_running, g_quit;
static void launch(int gpx) {
    if (S.is2000 && *get("gpxset.ini", "CommandLine", "SeasonSet", "")) put("gpxset.ini", "CommandLine", "SeasonSet", "");
    flush();
    lx_start_game(gpx);
    g_running = lx_game_running();
    if (g_running) { status(L("Jogo em execução…")); SDL_HideWindow(g_win); }
    else status(L(
#ifdef _WIN32
        "Não foi possível iniciar o jogo (veja gpxpatch-gp3rt.log)."
#else
        "Não foi possível iniciar o jogo (veja gp3linux-gpxpatch.log)."
#endif
        ));
}
static void frame(void) {
    int ww, wh; SDL_GetRendererOutputSize(g_ren, &ww, &wh);
    float W = (float)ww, H = (float)wh;
    if (nk_begin(ctx, "main", nk_rect(0, 0, W, H), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND | (A.open ? NK_WINDOW_ROM : 0))) {
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        float hh = U(56), fh = U(62), nw = U(200);
        /* header */
        { struct nk_rect a = space_row(hh, W, 4);
          nk_fill_rect(cv, a, 0, C_PANEL); nk_stroke_line(cv, a.x, a.y + hh - 1, a.x + a.w, a.y + hh - 1, U(1), C_LINE);
          float x = a.x + U(20); draw_text(cv, F_TITLE, x, a.y + U(15), U(60), "GPx", C_TEXT); x += text_w(F_TITLE, "GPx", -1);
          draw_text(cv, F_TITLE, x, a.y + U(15), U(80), "Patch", C_ACCENT); x += text_w(F_TITLE, "Patch ", -1);
          #ifdef _WIN32
          draw_text(cv, F_TITLE, x, a.y + U(15), U(200), "GP3RT", C_TEXT); x += text_w(F_TITLE, "GP3RT", -1) + U(18);
#else
          draw_text(cv, F_TITLE, x, a.y + U(15), U(200), L("para Linux"), C_TEXT); x += text_w(F_TITLE, L("para Linux"), -1) + U(18);
#endif
          char meta[PATH_MAX + 100]; snprintf(meta, sizeof meta, "%s  ·  %s  ·  GP3RT %s", S.is2000 ? "Grand Prix 3 2000" : "Grand Prix 3", lx_dir(), lx_version());
          draw_text(cv, F_SMALL, x, a.y + U(21), a.w - x - U(260), meta, C_MUTED);
          nk_layout_space_push(ctx, nk_rect(W - U(240), U(13), U(70), U(30))); nk_label_colored(ctx, L("Idioma"), NK_TEXT_RIGHT, C_MUTED);
          nk_layout_space_push(ctx, nk_rect(W - U(160), U(13), U(140), U(30)));
          static const opt lg[] = { { "pt", "Português" }, { "en", "English" } }; char nv[8];
          if (combo(g_en ? "en" : "pt", lg, 2, U(140), nv, sizeof nv)) { g_en = !strcmp(nv, "en"); put("gp3rt.ini", "launcher", "language", nv); } }
        nk_layout_space_end(ctx);
        /* body: nav + content */
        float bh = H - hh - fh;
        nk_layout_row_begin(ctx, NK_STATIC, bh, 2);
        nk_layout_row_push(ctx, nw);
        if (nk_group_begin(ctx, "nav", NK_WINDOW_NO_SCROLLBAR)) {
            struct nk_rect a = nk_window_get_content_region(ctx);
            nk_fill_rect(nk_window_get_canvas(ctx), nk_rect(a.x, a.y, a.w, bh), 0, C_PANEL);
            nk_stroke_line(nk_window_get_canvas(ctx), a.x + a.w - 1, a.y, a.x + a.w - 1, a.y + bh, U(1), C_LINE);
            space(U(10));
            for (int i = 0; i < NTABS; i++) {
                if (TABS[i].only2000 && !S.is2000) continue;
#ifdef _WIN32
                if (!strcmp(TABS[i].id, "gp3rt") && !S.ddraw) continue;   /* only with the GP3RT ddraw.dll */
#endif
                if (TABS[i].sep_before) { nk_layout_row_dynamic(ctx, U(17), 1); struct nk_rect r; if (nk_widget(&r, ctx)) nk_stroke_line(nk_window_get_canvas(ctx), r.x + U(14), r.y + U(8), r.x + r.w - U(14), r.y + U(8), U(1), C_LINE); }
                nk_layout_space_begin(ctx, NK_STATIC, U(40), 1);
                nk_layout_space_push(ctx, nk_rect(U(8), U(2), nw - U(17), U(36)));   /* (nk_widget below: absolute bounds) */
                struct nk_rect r; enum nk_widget_layout_states st = nk_widget(&r, ctx);
                if (st) {
                    int on = i == g_tab, hov = nk_input_is_mouse_hovering_rect(&ctx->input, r);
                    if (on || hov) nk_fill_rect(nk_window_get_canvas(ctx), r, U(7), on ? C_ACCENT : C_PANEL2);
                    draw_text(nk_window_get_canvas(ctx), on ? F_BOLD : F_TEXT, r.x + U(12), r.y + U(9), r.w - U(14), L(TABS[i].name), on ? nk_rgb(255, 255, 255) : C_TEXT);
                    if (st == NK_WIDGET_VALID && !B.open && !A.open && nk_input_is_mouse_click_in_rect(&ctx->input, NK_BUTTON_LEFT, r) && (i != g_tab || g_sub)) { g_tab = i; g_sub = 0; g_cs_edit[0] = 0; g_scroll_reset = 1; g_capture[0] = 0; }
                }
                nk_layout_space_end(ctx);
            }
            nk_group_end(ctx);
        }
        nk_layout_row_push(ctx, W - nw);
        if (g_scroll_reset) { nk_group_set_scroll(ctx, "content", 0, 0); g_scroll_reset = 0; }
        if (nk_group_begin(ctx, "content", 0)) {
            struct nk_rect cr = nk_window_get_content_region(ctx);
            float pad = U(22), maxw = U(980);
            g_cw = cr.w - 2 * pad; if (g_cw > maxw) g_cw = maxw;
            /* a centred column of g_cw */
            /* a column of g_cw inside the scrolling area (the panel bounds are restored before the end) */
            struct nk_panel *lp = ctx->current->layout; float ox = lp->bounds.x, ow = lp->bounds.w;
            lp->bounds.x += pad; lp->bounds.w = g_cw; lp->at_x = lp->bounds.x;
            space(U(16));
            nk_layout_row_dynamic(ctx, U(34), 1);
            { struct nk_rect r; if (nk_widget(&r, ctx)) { char tt[200]; snprintf(tt, sizeof tt, "%s", L(TABS[g_tab].name));
                if (!strcmp(TABS[g_tab].id, "set") && g_sub) { strcat(tt, "  \xc2\xbb  "); strcat(tt, L("Temporada")); if (g_sub == 2) { strcat(tt, "  \xc2\xbb  "); strcat(tt, L("Carset")); } }
                draw_text(nk_window_get_canvas(ctx), F_TITLE, r.x, r.y + U(2), r.w, tt, C_TEXT); } }
            if (!S.gpx && strcmp(TABS[g_tab].id, "gp3rt") && strcmp(TABS[g_tab].id, "about")) {
                if (lx_bundled_gpx()) {
                    notice(L("O GPxPatch 4.52 veio junto neste pacote (arquivos originais, sem nenhuma alteração, com autorização do autor, René Smit). Clique abaixo para colocá-lo na pasta do jogo. Nenhum arquivo que já esteja lá é substituído."));
                    nk_layout_row_static(ctx, U(34), (int)U(300), 1);
                    if (nk_button_label(ctx, L("Instalar o GPxPatch 4.52"))) {
                        int n = lx_install_bundled_gpx();
                        scan();
                        status(n >= 0 && S.gpx ? L("GPxPatch 4.52 instalado na pasta do jogo.") : L("Não foi possível instalar o GPxPatch na pasta do jogo."));
                    }
                } else notice(L("O GPxPatch não está instalado nesta pasta. Baixe o GPxPatch 4.52 em www.sdidit.nl, instale (pelo Wine ou num PC com Windows) e deixe o GPxPSup.dll e a pasta gpxpdata na pasta do jogo (veja o README / LEIAME). Depois abra esta tela de novo."));
            }
            else {
                if (S.gpx && !S.gpxsup && !strcmp(TABS[g_tab].id, "gpx")) notice(L("Falta o arquivo GPxPSup.dll na pasta do jogo: sem ele o GPxPatch não carrega."));
                TABS[g_tab].build();
            }
            space(U(4));
            lp->bounds.x = ox; lp->bounds.w = ow;
            nk_group_end(ctx);
        }
        nk_layout_row_end(ctx);
        /* footer */
        { struct nk_rect a = space_row(fh, W, 4);
          nk_fill_rect(cv, a, 0, C_PANEL); nk_stroke_line(cv, a.x, a.y, a.x + a.w, a.y, U(1), C_LINE);
          nk_layout_space_push(ctx, nk_rect(U(20), U(13), U(210), U(36)));
          nk_style_push_style_item(ctx, &ctx->style.button.normal, nk_style_item_color(C_ACCENT));
          nk_style_push_style_item(ctx, &ctx->style.button.hover, nk_style_item_color(C_ACCENT2));
          nk_style_push_color(ctx, &ctx->style.button.text_normal, nk_rgb(255, 255, 255));
          nk_style_push_color(ctx, &ctx->style.button.text_hover, nk_rgb(255, 255, 255));
          nk_style_push_color(ctx, &ctx->style.button.border_color, C_ACCENT);
          nk_style_push_font(ctx, FONT(F_BOLD));
          if (nk_button_label(ctx, L("Iniciar com GPxPatch")) && !g_running) launch(1);
          nk_style_pop_font(ctx); nk_style_pop_color(ctx); nk_style_pop_color(ctx); nk_style_pop_color(ctx); nk_style_pop_style_item(ctx); nk_style_pop_style_item(ctx);
          nk_layout_space_push(ctx, nk_rect(U(240), U(13), U(210), U(36)));
          if (nk_button_label(ctx, L("Iniciar sem GPxPatch")) && !g_running) launch(0);
          Uint32 now = SDL_GetTicks();
          if (g_saved_t && now - g_saved_t < 1600) draw_text(cv, F_SMALL, a.x + U(466), a.y + U(23), U(200), g_saved_fail ? Lf("{0} valor(es) não salvos", "1", 0, 0) : (g_en ? "\xe2\x9c\x93 saved" : "\xe2\x9c\x93 salvo"), g_saved_fail ? C_WARN : C_OK);
          if (g_status[0]) { float tw = text_w(F_SMALL, g_status, -1); if (tw > a.w - U(600)) tw = a.w - U(600); draw_text(cv, F_SMALL, a.x + a.w - tw - U(20), a.y + U(23), tw, g_status, C_MUTED); } }
        nk_layout_space_end(ctx);
        if (B.open || A.open) nk_fill_rect(cv, nk_rect(0, 0, W, H), 0, nk_rgba(0, 0, 0, 140));
        browse_draw(W, H);
    }
    nk_end(ctx);
    ask_draw(W, H);
}
/* off-screen pass: every tab once, so options missing from the GPx .ini files get the value shown */
static void dry_pass(void) {
    if (!S.gpx) return;
    g_dry = 1;
    for (int i = 0; i < NTABS; i++) if (strcmp(TABS[i].id, "gp3rt") && !(TABS[i].only2000 && !S.is2000)) TABS[i].build();
    g_dry = 0;
    flush();
}

int gpxui_main(const char *game_dir, const char *version) {
    lx_setup(game_dir, version);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { hlog("gpxui: SDL_Init failed: %s", SDL_GetError()); return -1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#ifdef SDL_HINT_APP_NAME
    SDL_SetHint(SDL_HINT_APP_NAME, "GPxPatch");
#endif
    g_win = SDL_CreateWindow(
#ifdef _WIN32
        "GPxPatch GP3RT",
#else
        "GPxPatch",
#endif
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1180, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
    if (!g_win) { hlog("gpxui: no window: %s", SDL_GetError()); SDL_Quit(); return -1; }
    set_window_icon_gpxp(g_win);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren || !(ctx = nk_sdl_init(g_win, g_ren))) { hlog("gpxui: no renderer or SDL < 2.0.18: %s", SDL_GetError()); if (g_ren) SDL_DestroyRenderer(g_ren); SDL_DestroyWindow(g_win); SDL_Quit(); return -1; }
    { int ow, oh, w, h; SDL_GetRendererOutputSize(g_ren, &ow, &oh); SDL_GetWindowSize(g_win, &w, &h); g_sc = w ? (float)ow / (float)w : 1; if (g_sc < 1) g_sc = 1;
      const char *z = getenv("GP3_UI_SCALE"); if (z && atof(z) > 0.5) g_sc *= (float)atof(z); }
    nk_sdl_font_stash_begin();
    g_font[F_TEXT] = nk_sdl_add_font(g_inter_regular, (int)sizeof g_inter_regular, U(15));
    g_font[F_SMALL] = nk_sdl_add_font(g_inter_regular, (int)sizeof g_inter_regular, U(12.5));
    g_font[F_BOLD] = nk_sdl_add_font(g_inter_semibold, (int)sizeof g_inter_semibold, U(15));
    g_font[F_TITLE] = nk_sdl_add_font(g_inter_semibold, (int)sizeof g_inter_semibold, U(20));
    g_font[F_H3] = nk_sdl_add_font(g_inter_semibold, (int)sizeof g_inter_semibold, U(12));
    nk_sdl_font_stash_end();
    nk_style_set_font(ctx, FONT(F_TEXT));
    theme();
    scan();
    const char *lang = get("gp3rt.ini", "launcher", "language", "");
    if (*lang) g_en = !strcmp(lang, "en");
    else { const char *e = getenv("LANG"); g_en = !(e && !strncasecmp(e, "pt", 2)); }
    { const char *t = get("gp3rt.ini", "launcher", "tab", ""); for (int i = 0; i < NTABS; i++) if (!strcmp(TABS[i].id, t) && !(TABS[i].only2000 && !S.is2000)) g_tab = i; }
    g_cw = U(900);
    dry_pass();
    hlog("gpxui: native GPxPatch screen (%s, scale %.2f)", lx_dir(), g_sc);
#ifndef _WIN32
    {   /* our icon: launchers in the game folder (+ the custom icon on gp3 / GPxPatch where the file manager supports it) */
        extern void set_folder_icons(const char *);
        lx_folder_launchers();
        char self[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n > 0) { self[n] = 0; char *sl = strrchr(self, '/'); if (sl) *sl = 0; set_folder_icons(self); }
    }
#endif
    SDL_ShowWindow(g_win);
    SDL_StartTextInput();
    int frames = 3; Uint32 last_poll = 0;
    while (!g_quit) {
        SDL_Event e; int got = 0;
        nk_input_begin(ctx);
        if (SDL_WaitEventTimeout(&e, frames > 0 ? 10 : 250)) {
            do {
                got = 1;
                if (getenv("GP3_UI_DEBUG") && e.type == SDL_MOUSEBUTTONDOWN) hlog("click %d,%d", e.button.x, e.button.y);
                if (getenv("GP3_UI_DEBUG") && (e.type == SDL_KEYDOWN || e.type == SDL_TEXTINPUT)) hlog("ev %x %s", e.type, e.type == SDL_TEXTINPUT ? e.text.text : SDL_GetKeyName(e.key.keysym.sym));
                if (e.type == SDL_QUIT) g_quit = 1;
                else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) g_quit = 1;
                else if (e.type == SDL_KEYDOWN && g_capture[0] && !e.key.repeat) { key_captured(e.key.keysym.scancode); continue; }
                else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_s && (SDL_GetModState() & KMOD_CTRL) && !g_running) { launch(1); continue; }
                nk_sdl_handle_event(&e, g_sc);
            } while (SDL_PollEvent(&e));
        }
        nk_input_end(ctx);
        Uint32 now = SDL_GetTicks();
        if (g_pend && now - g_pend_t > 300) flush();
        if (now - last_poll > 500) {
            last_poll = now;
            if (g_running && !lx_game_running()) {
                g_running = 0; int x = lx_last_exit();
                char b[16]; snprintf(b, sizeof b, "%d", x);
                
#ifdef _WIN32
                status(L("Jogo encerrado."));
#else
                status(x > 0 ? Lf("O jogo fechou com erro ({0}). Veja gp3linux-game.log na pasta do jogo.", b, 0, 0) : L("Jogo encerrado."));
#endif
                ini_forget_all(); scan(); g_nex = 0;
                SDL_ShowWindow(g_win); SDL_RaiseWindow(g_win); frames = 3;
            }
        }
        if (got) frames = 3;
        if (frames > 0 || got || (now - g_saved_t < 1700)) {
            frame();
            SDL_SetRenderDrawColor(g_ren, C_BG.r, C_BG.g, C_BG.b, 255); SDL_RenderClear(g_ren);
            nk_sdl_render();
            SDL_RenderPresent(g_ren);
            if (frames > 0) frames--;
        }   /* (no nk_clear without a frame: it would drop every window and the focused text field) */
    }
    put("gp3rt.ini", "launcher", "tab", TABS[g_tab].id);
    flush();
    nk_sdl_shutdown(); SDL_DestroyRenderer(g_ren); SDL_DestroyWindow(g_win); SDL_Quit();
    hlog("gpxui: closed");
    return 0;
}
