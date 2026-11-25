/* Minimal GDI: DIB sections loaded from BMP files, memory DCs, DCs onto
 * DirectDraw surfaces, BitBlt/StretchBlt with format conversion.
 * Printing entry points fail gracefully. */
#include "rt.h"
#include "gdi.h"
#include "ttf.h"
#include <math.h>

#define GDI_BASE 0x00440000u
#define GDI_MAX 1024
typedef struct { int type; union { gdi_bitmap_t bmp; gdi_dc_t dc; gdi_font_t font; } u; } gdiobj_t;
enum { GO_FREE, GO_BITMAP, GO_DC, GO_FONT, GO_PEN, GO_RGN, GO_STOCK };
static gdiobj_t g_objs[GDI_MAX];

static uint32_t obj_new(int type) {
    for (int i = 1; i < GDI_MAX; i++) if (!g_objs[i].type) { memset(&g_objs[i], 0, sizeof g_objs[i]); g_objs[i].type = type; return GDI_BASE + i * 4; }
    return 0;
}
static gdiobj_t *obj_get(uint32_t h, int type) {
    if (h < GDI_BASE) return NULL;
    uint32_t i = (h - GDI_BASE) / 4;
    if (i >= GDI_MAX || g_objs[i].type == GO_FREE) return NULL;
    if (type && g_objs[i].type != type) return NULL;
    return &g_objs[i];
}
gdi_dc_t *gdi_get_dc(uint32_t h) { gdiobj_t *o = obj_get(h, GO_DC); return o ? &o->u.dc : NULL; }

uint32_t gdi_surface_dc(void *bits, int w, int h, int pitch, int bpp, const uint32_t *pal, const uint32_t masks[3]) {
    uint32_t hd = obj_new(GO_DC);
    gdi_dc_t *d = gdi_get_dc(hd);
    d->surf.bits = bits; d->surf.w = w; d->surf.h = h; d->surf.pitch = pitch; d->surf.bpp = bpp;
    if (pal) memcpy(d->surf.pal, pal, 256 * 4);
    if (masks) memcpy(d->surf.masks, masks, sizeof d->surf.masks);
    d->is_surface = 1; d->bkcolor = 0xffffff; d->bkmode = 2;
    return hd;
}
void gdi_free_dc(uint32_t h) { gdiobj_t *o = obj_get(h, GO_DC); if (o) o->type = GO_FREE; }

uint32_t gdi_window_dc(HWND h) {
    (void)h;
    static uint32_t wdc;
    if (!wdc) { wdc = obj_new(GO_DC); gdi_get_dc(wdc)->is_window = 1; }
    return wdc;
}

/* ------------------------------------------------------------------ */
/* BMP loading                                                          */
uint32_t gdi_load_image(const char *file, UINT type, int cx, int cy, UINT fl) {
    (void)cx; (void)cy;
    if (type != 0) { LOG("LoadImageA(%s): only bitmaps supported", file); return 0; }
    char hp[1024];
    extern int win_host_path(const char *, char *, size_t);
    if (!win_host_path(file, hp, sizeof hp)) { LOG("LoadImageA(%s): not found", file); return 0; }
    FILE *f = fopen(hp, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n); fread(d, 1, n, f); fclose(f);
    if (n < 54 || d[0] != 'B' || d[1] != 'M') { free(d); return 0; }
    uint32_t off = *(uint32_t *)(d + 10);
    uint32_t hsz = *(uint32_t *)(d + 14);
    int32_t w = *(int32_t *)(d + 18), h = *(int32_t *)(d + 22);
    int bpp = *(uint16_t *)(d + 28);
    uint32_t comp = *(uint32_t *)(d + 30), clrused = *(uint32_t *)(d + 46);
    if (comp != 0 && comp != 3) { LOG("LoadImageA(%s): compressed bmp unsupported", file); free(d); return 0; }
    uint32_t hb = obj_new(GO_BITMAP);
    gdi_bitmap_t *b = &obj_get(hb, GO_BITMAP)->u.bmp;
    b->w = w; b->h = h < 0 ? -h : h; b->bpp = bpp; b->bottom_up = h > 0;
    b->pitch = ((w * bpp + 31) / 32) * 4;
    b->bits = calloc(1, (size_t)b->pitch * b->h);
    memcpy(b->bits, d + off, (size_t)b->pitch * b->h <= (size_t)(n - off) ? (size_t)b->pitch * b->h : (size_t)(n - off));
    if (bpp <= 8) {
        int nc = clrused ? (int)clrused : 1 << bpp;
        uint8_t *pal = d + 14 + hsz;
        for (int i = 0; i < nc && i < 256; i++) b->pal[i] = pal[i * 4] | (pal[i * 4 + 1] << 8) | (pal[i * 4 + 2] << 16);
        b->ncolors = nc;
    }
    if (bpp == 16) { b->masks[0] = 0x7c00; b->masks[1] = 0x3e0; b->masks[2] = 0x1f; if (comp == 3) memcpy(b->masks, d + 54, 12); }
    free(d);
    TRACE("LoadImageA(%s) -> %x (%dx%d %dbpp)", file, hb, w, b->h, bpp);
    (void)fl;
    return hb;
}

/* ------------------------------------------------------------------ */
/* pixel access                                                         */
static int pix_source(uint32_t hdc, gdi_view_t *v) {
    gdi_dc_t *d = gdi_get_dc(hdc);
    if (!d) return 0;
    if (d->is_surface) { *v = d->surf; return 1; }
    gdiobj_t *o = obj_get(d->bitmap, GO_BITMAP);
    if (!o) return 0;
    gdi_bitmap_t *b = &o->u.bmp;
    v->w = b->w; v->h = b->h; v->bpp = b->bpp;
    if (b->bottom_up) { v->bits = b->bits + (size_t)(b->h - 1) * b->pitch; v->pitch = -b->pitch; }
    else { v->bits = b->bits; v->pitch = b->pitch; }
    memcpy(v->pal, b->pal, sizeof v->pal);
    memcpy(v->masks, b->masks, sizeof v->masks);
    return 1;
}
static uint32_t get_rgb(gdi_view_t *v, int x, int y, int *index) {
    uint8_t *row = (uint8_t *)v->bits + (ptrdiff_t)y * v->pitch;
    switch (v->bpp) {
    case 8: { int i = row[x]; if (index) *index = i; return v->pal[i]; }
    case 16: {
        uint16_t p = ((uint16_t *)row)[x];
        if (v->masks[1] == 0x7e0) return ((p >> 11) & 31) * 255 / 31 << 16 | ((p >> 5) & 63) * 255 / 63 << 8 | (p & 31) * 255 / 31;
        return ((p >> 10) & 31) * 255 / 31 << 16 | ((p >> 5) & 31) * 255 / 31 << 8 | (p & 31) * 255 / 31;
    }
    case 24: return row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16;
    case 32: return ((uint32_t *)row)[x] & 0xffffff;
    }
    return 0;
}
static int nearest_index(const uint32_t *pal, uint32_t rgb) {
    int best = 0, bd = 1 << 30;
    int r = rgb & 255, g = (rgb >> 8) & 255, b = (rgb >> 16) & 255;
    for (int i = 0; i < 256; i++) {
        int dr = r - (int)(pal[i] & 255), dg = g - (int)((pal[i] >> 8) & 255), db = b - (int)((pal[i] >> 16) & 255);
        int dd = dr * dr + dg * dg + db * db;
        if (dd < bd) { bd = dd; best = i; if (!dd) break; }
    }
    return best;
}
/* all colors handled here are 0x00RRGGBB (palettes included) */
static void put_rgb(gdi_view_t *v, int x, int y, uint32_t xrgb, int index) {
    uint8_t *row = (uint8_t *)v->bits + (ptrdiff_t)y * v->pitch;
    switch (v->bpp) {
    case 8: row[x] = (uint8_t)(index >= 0 ? index : nearest_index(v->pal, xrgb)); break;
    case 16: {
        int r = (xrgb >> 16) & 255, g = (xrgb >> 8) & 255, b = xrgb & 255;
        ((uint16_t *)row)[x] = v->masks[1] == 0x7e0 ? (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3)
                                                    : (uint16_t)((r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
        break;
    }
    case 24: row[x * 3] = xrgb & 255; row[x * 3 + 1] = (xrgb >> 8) & 255; row[x * 3 + 2] = (xrgb >> 16) & 255; break;
    case 32: ((uint32_t *)row)[x] = xrgb; break;
    }
}

static BOOL do_blt(uint32_t dst, int dx, int dy, int dw, int dh, uint32_t src, int sx, int sy, int sw, int sh, DWORD rop) {
    gdi_view_t d, s;
    if (!pix_source(dst, &d)) { TRACE("blt: bad dst dc %x", dst); return TRUE; }
    if (rop == 0x42 || rop == 0xff0062) {   /* BLACKNESS / WHITENESS */
        for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
            int X = dx + x, Y = dy + y; if (X < 0 || Y < 0 || X >= d.w || Y >= d.h) continue;
            put_rgb(&d, X, Y, rop == 0x42 ? 0 : 0xffffff, -1);
        }
        return TRUE;
    }
    if (!pix_source(src, &s)) { TRACE("blt: bad src dc %x", src); return FALSE; }
    int same_pal = d.bpp == 8 && s.bpp == 8;
    for (int y = 0; y < dh; y++) {
        int Y = dy + y; if (Y < 0 || Y >= d.h) continue;
        int SY = sy + (int)((int64_t)y * sh / dh); if (SY < 0 || SY >= s.h) continue;
        for (int x = 0; x < dw; x++) {
            int X = dx + x; if (X < 0 || X >= d.w) continue;
            int SX = sx + (int)((int64_t)x * sw / dw); if (SX < 0 || SX >= s.w) continue;
            int idx = -1;
            uint32_t c = get_rgb(&s, SX, SY, &idx);
            put_rgb(&d, X, Y, c, same_pal ? idx : -1);
        }
    }
    return TRUE;
}

static BOOL WINAPI BitBlt_(uint32_t dst, int x, int y, int w, int h, uint32_t src, int sx, int sy, DWORD rop) {
    return do_blt(dst, x, y, w, h, src, sx, sy, w, h, rop);
}
static BOOL WINAPI StretchBlt_(uint32_t dst, int x, int y, int w, int h, uint32_t src, int sx, int sy, int sw, int sh, DWORD rop) {
    return do_blt(dst, x, y, w, h, src, sx, sy, sw, sh, rop);
}
static int WINAPI SetStretchBltMode_(uint32_t dc, int m) { (void)dc; (void)m; return 1; }

static uint32_t WINAPI CreateCompatibleDC_(uint32_t dc) { (void)dc; uint32_t h = obj_new(GO_DC); gdi_dc_t *d = gdi_get_dc(h); if (d) { d->bkcolor = 0xffffff; d->bkmode = 2; } return h; }
static BOOL WINAPI DeleteDC_(uint32_t dc) { gdiobj_t *o = obj_get(dc, GO_DC); if (o) o->type = GO_FREE; return TRUE; }
static uint32_t WINAPI SelectObject_(uint32_t dc, uint32_t obj) {
    gdi_dc_t *d = gdi_get_dc(dc);
    if (!d) return 0;
    gdiobj_t *o = obj_get(obj, 0);
    if (o && o->type == GO_BITMAP) { uint32_t old = d->bitmap ? d->bitmap : 0x00440ffcu; d->bitmap = obj; return old; }
    if (o && o->type == GO_FONT) { uint32_t old = d->font; d->font = obj; return old ? old : obj; }
    return obj ? obj : 1;
}
static BOOL WINAPI DeleteObject_(uint32_t h) {
    gdiobj_t *o = obj_get(h, 0);
    if (!o) return TRUE;
    if (o->type == GO_BITMAP) free(o->u.bmp.bits);
    if (o->type != GO_STOCK) o->type = GO_FREE;
    return TRUE;
}
static int WINAPI GetObjectA_(uint32_t h, int n, void *out) {
    gdiobj_t *o = obj_get(h, 0);
    if (!o) return 0;
    if (o->type == GO_BITMAP) {
        gdi_bitmap_t *b = &o->u.bmp;
        uint32_t bm[6] = { 0, (uint32_t)b->w, (uint32_t)b->h, (uint32_t)b->pitch, (uint32_t)(1 | (b->bpp << 16)), (uint32_t)(uintptr_t)b->bits };
        int m = n < 24 ? n : 24;
        memcpy(out, bm, m);
        if (n >= 84) {   /* DIBSECTION: BITMAP + BITMAPINFOHEADER + masks + handle + offset */
            uint32_t *dib = (uint32_t *)out;
            dib[6] = 40; dib[7] = b->w; dib[8] = b->bottom_up ? b->h : -b->h; dib[9] = 1 | (b->bpp << 16);
            dib[10] = 0; dib[11] = b->pitch * b->h; dib[12] = dib[13] = 0; dib[14] = b->ncolors; dib[15] = 0;
            dib[16] = b->masks[0]; dib[17] = b->masks[1]; dib[18] = b->masks[2]; dib[19] = 0; dib[20] = 0;
            m = 84;
        }
        return m;
    }
    return 0;
}
static UINT WINAPI GetDIBColorTable_(uint32_t dc, UINT start, UINT n, uint8_t *out) {
    gdi_dc_t *d = gdi_get_dc(dc); if (!d) return 0;
    gdiobj_t *o = obj_get(d->bitmap, GO_BITMAP); if (!o) return 0;
    gdi_bitmap_t *b = &o->u.bmp;
    UINT k = 0;
    for (; k < n && start + k < 256 && (int)(start + k) < b->ncolors; k++) {
        uint32_t c = b->pal[start + k];
        out[k * 4] = c & 255; out[k * 4 + 1] = (c >> 8) & 255; out[k * 4 + 2] = (c >> 16) & 255; out[k * 4 + 3] = 0;
    }
    return k;
}
static int WINAPI GetDeviceCaps_(uint32_t dc, int idx) {
    (void)dc;
    switch (idx) {
    case 8: return 640; case 10: return 480;      /* HORZRES/VERTRES */
    case 12: return 32;                          /* BITSPIXEL */
    case 14: return 1;                           /* PLANES */
    case 24: return 256;                         /* NUMCOLORS */
    case 38: return 0x7e99;                      /* RASTERCAPS */
    case 88: case 90: return 96;                 /* LOGPIXELSX/Y */
    case 104: return 256;                        /* SIZEPALETTE */
    case 108: return 24;                         /* COLORRES */
    }
    return 0;
}
static uint32_t WINAPI GetStockObject_(int i) { (void)i; return GDI_BASE + (1000 + (i & 15)) * 4; }
static uint32_t WINAPI CreatePen_(int s, int w, DWORD c) { (void)s; (void)w; (void)c; return obj_new(GO_PEN); }
static uint32_t WINAPI CreateRectRgn_(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; return obj_new(GO_RGN); }
static int WINAPI SelectClipRgn_(uint32_t dc, uint32_t r) { (void)dc; (void)r; return 2; }
static int WINAPI SetMapMode_(uint32_t dc, int m) { (void)dc; (void)m; return 1; }
static BOOL WINAPI MoveToEx_(uint32_t dc, int x, int y, void *p) { (void)dc; (void)x; (void)y; (void)p; return TRUE; }
static BOOL WINAPI LineTo_(uint32_t dc, int x, int y) { (void)dc; (void)x; (void)y; return TRUE; }
static uint32_t WINAPI CreateDCA_(const char *d, const char *dev, const char *o, void *dm) { (void)d; (void)dev; (void)o; (void)dm; LOG("CreateDCA(printer) -> unavailable"); return 0; }
static uint32_t WINAPI CreateICA_(const char *d, const char *dev, const char *o, void *dm) { (void)d; (void)dev; (void)o; (void)dm; return 0; }
static int WINAPI StartDocA_(uint32_t dc, void *di) { (void)dc; (void)di; return -1; }
static int WINAPI EndDoc_(uint32_t dc) { (void)dc; return -1; }
static int WINAPI StartPage_(uint32_t dc) { (void)dc; return -1; }
static int WINAPI EndPage_(uint32_t dc) { (void)dc; return -1; }
static UINT WINAPI GetSystemPaletteUse_(uint32_t dc) { (void)dc; return 1; }
static UINT WINAPI SetSystemPaletteUse_(uint32_t dc, UINT u) { (void)dc; (void)u; return 1; }
static UINT WINAPI GetSystemPaletteEntries_(uint32_t dc, UINT s, UINT n, uint8_t *out) {
    (void)dc;
    for (UINT i = 0; i < n; i++) { uint32_t v = s + i; out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = (uint8_t)v; out[i * 4 + 3] = 0; }
    return n;
}
static uint32_t WINAPI CreateCompatibleBitmap_(uint32_t dc, int w, int h) {
    (void)dc;
    uint32_t hb = obj_new(GO_BITMAP);
    gdi_bitmap_t *b = &obj_get(hb, GO_BITMAP)->u.bmp;
    b->w = w; b->h = h; b->bpp = 32; b->pitch = w * 4; b->bits = calloc(1, (size_t)w * h * 4);
    return hb;
}
static uint32_t WINAPI CreateDIBSection_(uint32_t dc, const uint8_t *bmi, UINT usage, void **bits, HANDLE sec, DWORD off) {
    (void)dc; (void)usage; (void)sec; (void)off;
    int32_t w = *(int32_t *)(bmi + 4), h = *(int32_t *)(bmi + 8); int bpp = *(uint16_t *)(bmi + 14);
    uint32_t hb = obj_new(GO_BITMAP);
    gdi_bitmap_t *b = &obj_get(hb, GO_BITMAP)->u.bmp;
    b->w = w; b->h = h < 0 ? -h : h; b->bpp = bpp; b->bottom_up = h > 0;
    b->pitch = ((w * bpp + 31) / 32) * 4; b->bits = calloc(1, (size_t)b->pitch * b->h);
    uint32_t comp = *(uint32_t *)(bmi + 16);
    if (bpp == 16) {
        if (comp == 3) memcpy(b->masks, bmi + 40, 12);          /* BI_BITFIELDS */
        else { b->masks[0] = 0x7c00; b->masks[1] = 0x3e0; b->masks[2] = 0x1f; }
    }
    if (bpp <= 8) {   /* colour table follows the header */
        uint32_t n = *(uint32_t *)(bmi + 32); if (!n) n = 1u << bpp;
        for (uint32_t i = 0; i < n && i < 256; i++) b->pal[i] = *(uint32_t *)(bmi + 40 + i * 4) & 0xffffff;
        b->ncolors = (int)n;
    }
    if (bits) *bits = b->bits;
    return hb;
}
/* reads a bitmap as a DIB (GPxPatch screenshots: BitBlt from the screen into a compatible
   bitmap, then GetDIBits into a 24/32-bit buffer that is written to a .bmp) */
static int WINAPI GetDIBits_(uint32_t dc, uint32_t bm, UINT start, UINT n, void *bits, void *bmi_, UINT usage) {
    (void)dc; (void)usage;
    gdiobj_t *o = obj_get(bm, GO_BITMAP); uint8_t *bmi = bmi_;
    if (!o || !bmi) return 0;
    gdi_bitmap_t *b = &o->u.bmp;
    if (!bits) {   /* only fill in the header */
        int bpp = *(uint16_t *)(bmi + 14) ? *(uint16_t *)(bmi + 14) : (b->bpp < 24 ? 24 : b->bpp);
        *(int32_t *)(bmi + 4) = b->w; *(int32_t *)(bmi + 8) = b->h; *(uint16_t *)(bmi + 12) = 1; *(uint16_t *)(bmi + 14) = (uint16_t)bpp;
        *(uint32_t *)(bmi + 16) = 0; *(uint32_t *)(bmi + 20) = (uint32_t)(((b->w * bpp + 31) / 32) * 4 * b->h);
        return b->h;
    }
    int bpp = *(uint16_t *)(bmi + 14); int32_t dh = *(int32_t *)(bmi + 8);
    if (bpp != 24 && bpp != 32 && bpp != 16) return 0;
    gdi_view_t v; v.w = b->w; v.h = b->h; v.bpp = b->bpp;
    if (b->bottom_up) { v.bits = b->bits + (size_t)(b->h - 1) * b->pitch; v.pitch = -b->pitch; } else { v.bits = b->bits; v.pitch = b->pitch; }
    memcpy(v.pal, b->pal, sizeof v.pal); memcpy(v.masks, b->masks, sizeof v.masks);
    int H = dh < 0 ? -dh : dh, pitch = ((b->w * bpp + 31) / 32) * 4; UINT done = 0;
    for (UINT k = 0; k < n; k++) {
        int line = (int)(start + k); if (line >= H || line >= b->h) break;
        int sy = dh > 0 ? b->h - 1 - line : line;   /* bottom-up DIB: scan line 0 is the bottom row */
        uint8_t *dst = (uint8_t *)bits + (size_t)k * pitch;
        for (int x = 0; x < b->w; x++) {
            uint32_t c = get_rgb(&v, x, sy, NULL);
            if (bpp == 32) ((uint32_t *)dst)[x] = c;
            else if (bpp == 24) { dst[x * 3] = c & 255; dst[x * 3 + 1] = (c >> 8) & 255; dst[x * 3 + 2] = (c >> 16) & 255; }
            else ((uint16_t *)dst)[x] = (uint16_t)(((c >> 19) & 31) << 10 | ((c >> 11) & 31) << 5 | ((c >> 3) & 31));
        }
        done++;
    }
    return (int)done;
}
static DWORD WINAPI SetPixel_(uint32_t dc, int x, int y, DWORD c) { (void)dc; (void)x; (void)y; return c; }
static DWORD WINAPI GetPixel_(uint32_t dc, int x, int y) { (void)dc; (void)x; (void)y; return 0; }


/* ------------------------------------------------------------------ */
/* fonts and text (TrueType files: AddFontResource, the game folder, then the system) */
typedef struct { char family[64]; char path[600]; ttf_t *ttf; } regfont_t;
static regfont_t g_regfonts[64]; static int g_nregfonts;
extern int win_host_path(const char *, char *, size_t);
static int WINAPI AddFontResourceA_(const char *file) {
    char hp[1024];
    if (!file || !win_host_path(file, hp, sizeof hp)) { LOG("AddFontResourceA(%s): not found", file ? file : "-"); return 0; }
    for (int i = 0; i < g_nregfonts; i++) if (!strcmp(g_regfonts[i].path, hp)) return 1;
    ttf_t *t = ttf_load(hp);
    if (!t || g_nregfonts >= 64) { LOG("AddFontResourceA(%s): not a TrueType font", file); return 0; }
    regfont_t *r = &g_regfonts[g_nregfonts++]; r->ttf = t; snprintf(r->path, sizeof r->path, "%s", hp); snprintf(r->family, sizeof r->family, "%s", t->family);
    LOG("font '%s' added from %s", t->family, file);
    return 1;
}
static int WINAPI AddFontResourceExA_(const char *file, DWORD fl, void *res) { (void)fl; (void)res; return AddFontResourceA_(file); }
static BOOL WINAPI RemoveFontResourceA_(const char *file) { (void)file; return TRUE; }
static BOOL WINAPI RemoveFontResourceExA_(const char *file, DWORD fl, void *res) { (void)file; (void)fl; (void)res; return TRUE; }
/* Windows font names -> metric-compatible Linux fonts (Liberation), then DejaVu / FreeFont / Noto */
static ttf_t *try_fonts(const char *const *names) {
    static const char *dirs[] = { "/usr/share/fonts/truetype/liberation/", "/usr/share/fonts/truetype/liberation2/", "/usr/share/fonts/liberation-sans/",
        "/usr/share/fonts/liberation-mono/", "/usr/share/fonts/liberation-serif/", "/usr/share/fonts/TTF/", "/usr/share/fonts/truetype/dejavu/",
        "/usr/share/fonts/dejavu/", "/usr/share/fonts/truetype/freefont/", "/usr/share/fonts/truetype/noto/", "/usr/share/fonts/noto/", NULL };
    for (const char *const *n = names; *n; n++)
        for (const char **d = dirs; *d; d++) { char p[300]; snprintf(p, sizeof p, "%s%s", *d, *n); ttf_t *t = ttf_load(p); if (t) return t; }
    return NULL;
}
static ttf_t *sys_font_for(const char *face, int bold, int italic) {
    enum { SANS, SERIF, MONO } kind = SANS;
    if (strcasestr(face, "courier") || strcasestr(face, "mono") || strcasestr(face, "fixed") || strcasestr(face, "terminal") || strcasestr(face, "console")) kind = MONO;
    else if (strcasestr(face, "times") || strcasestr(face, "serif") || strcasestr(face, "georgia") || strcasestr(face, "roman")) kind = SERIF;
    static ttf_t *cache[3][4]; int v = (bold ? 1 : 0) | (italic ? 2 : 0);
    if (cache[kind][v]) return cache[kind][v];
    static const char *const sans[4][4] = { { "LiberationSans-Regular.ttf", "DejaVuSans.ttf", "FreeSans.ttf", NULL }, { "LiberationSans-Bold.ttf", "DejaVuSans-Bold.ttf", "FreeSansBold.ttf", NULL },
        { "LiberationSans-Italic.ttf", "DejaVuSans-Oblique.ttf", "FreeSansOblique.ttf", NULL }, { "LiberationSans-BoldItalic.ttf", "DejaVuSans-BoldOblique.ttf", "FreeSansBoldOblique.ttf", NULL } };
    static const char *const serif[4][4] = { { "LiberationSerif-Regular.ttf", "DejaVuSerif.ttf", "FreeSerif.ttf", NULL }, { "LiberationSerif-Bold.ttf", "DejaVuSerif-Bold.ttf", "FreeSerifBold.ttf", NULL },
        { "LiberationSerif-Italic.ttf", "DejaVuSerif-Italic.ttf", "FreeSerifItalic.ttf", NULL }, { "LiberationSerif-BoldItalic.ttf", "DejaVuSerif-BoldItalic.ttf", "FreeSerifBoldItalic.ttf", NULL } };
    static const char *const mono[4][4] = { { "LiberationMono-Regular.ttf", "DejaVuSansMono.ttf", "FreeMono.ttf", NULL }, { "LiberationMono-Bold.ttf", "DejaVuSansMono-Bold.ttf", "FreeMonoBold.ttf", NULL },
        { "LiberationMono-Italic.ttf", "DejaVuSansMono-Oblique.ttf", "FreeMonoOblique.ttf", NULL }, { "LiberationMono-BoldItalic.ttf", "DejaVuSansMono-BoldOblique.ttf", "FreeMonoBoldOblique.ttf", NULL } };
    const char *const *list = kind == MONO ? mono[v] : kind == SERIF ? serif[v] : sans[v];
    ttf_t *t = try_fonts(list);
    if (!t && v) t = sys_font_for(face, 0, 0);   /* no styled file: the regular one (synthetic bold/italic) */
    if (!t && kind != SANS) t = sys_font_for("", bold, italic);
    if (!t) {   /* the game folder's own fonts (GPxPatch ships some) */
        char hp[1024]; if (win_host_path("gpxpdata\\gfx\\fonts\\din-bold.ttf", hp, sizeof hp)) t = ttf_load(hp);
    }
    if (t) LOG("text: '%s'%s%s -> system font '%s'", face, bold ? " bold" : "", italic ? " italic" : "", t->family);
    else LOG("text: no TrueType font found for '%s'", face);
    cache[kind][v] = t;
    return t;
}
static gdi_font_t *font_of(uint32_t h) {
    gdiobj_t *o = obj_get(h, GO_FONT);
    if (!o) {   /* stock fonts and the default */
        static gdiobj_t def; if (!def.type) { def.type = GO_FONT; def.u.font.height = -12; snprintf(def.u.font.face, 64, "System"); }
        o = &def;
    }
    gdi_font_t *f = &o->u.font;
    if (!f->resolved) {
        f->resolved = 1;
        for (int i = 0; i < g_nregfonts && !f->ttf; i++) if (!strcasecmp(g_regfonts[i].family, f->face)) f->ttf = g_regfonts[i].ttf;
        /* a face may also be asked by its file name (e.g. "din-bold") */
        for (int i = 0; i < g_nregfonts && !f->ttf; i++) { const char *b = strrchr(g_regfonts[i].path, '/'); b = b ? b + 1 : g_regfonts[i].path; if (!strncasecmp(b, f->face, strlen(f->face)) && f->face[0]) f->ttf = g_regfonts[i].ttf; }
        if (!f->ttf) f->ttf = sys_font_for(f->fixed && !f->face[0] ? "courier" : f->face, f->weight >= 600, f->italic);
        TRACE("font '%s' %d -> %s", f->face, f->height, f->ttf ? f->ttf->family : "(none)");
    }
    return f;
}
/* pixel size of the em square, ascent and descent (GDI uses the OS/2 win metrics) */
static void font_metrics(gdi_font_t *f, float *em, int *asc, int *desc) {
    ttf_t *t = f->ttf;
    if (!t) { *em = 12; *asc = 10; *desc = 3; return; }
    float cell = (float)(t->win_ascent + t->win_descent); if (cell <= 0) cell = (float)t->upem;
    float e;
    if (f->height < 0) e = (float)-f->height;
    else if (f->height > 0) e = f->height * t->upem / cell;
    else e = 12.0f;
    *em = e;
    *asc = (int)ceilf(t->win_ascent * e / t->upem); *desc = (int)ceilf(t->win_descent * e / t->upem);
}
static uint32_t cp1252_uni(unsigned char c) {
    static const uint16_t hi[32] = {0x20AC,0x81,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,0x2C6,0x2030,0x160,0x2039,0x152,0x8D,0x17D,0x8F,
                                    0x90,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x2DC,0x2122,0x161,0x203A,0x153,0x9D,0x17E,0x178};
    return (c >= 0x80 && c < 0xa0) ? hi[c - 0x80] : c;
}
static int text_width(gdi_font_t *f, const char *s, int n, const int *dx) {
    float em; int a, d; font_metrics(f, &em, &a, &d);
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (dx) { w += dx[i]; continue; }
        if (!f->ttf) { w += (int)(em / 2); continue; }
        int g = ttf_glyph_index(f->ttf, cp1252_uni((unsigned char)s[i]));
        w += (int)lroundf(ttf_advance(f->ttf, g) * em / f->ttf->upem) + ((f->weight >= 600 && !f->ttf->bold) ? 1 : 0);
    }
    return w;
}
static uint32_t colorref_rgb(uint32_t c) { return (c & 0xff) << 16 | (c & 0xff00) | (c >> 16 & 0xff); }
static void fill_rect(gdi_view_t *v, int x0, int y0, int x1, int y1, uint32_t rgb) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > v->w) x1 = v->w; if (y1 > v->h) y1 = v->h;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) put_rgb(v, x, y, rgb, -1);
}
static void draw_text(uint32_t hdc, int x, int y, const char *s, int n, const int32_t *clip, int opaque_rect, const int *dx) {
    gdi_dc_t *d = gdi_get_dc(hdc); gdi_view_t v;
    if (!d || !pix_source(hdc, &v)) return;
    gdi_font_t *f = font_of(d->font);
    float em; int asc, desc; font_metrics(f, &em, &asc, &desc);
    int w = text_width(f, s, n, dx);
    int al = d->align;
    if ((al & 6) == 6) x -= w / 2; else if (al & 2) x -= w;
    int base = (al & 24) == 24 ? y : (al & 8) ? y - desc : y + asc;   /* BASELINE / BOTTOM / TOP */
    int cx0 = 0, cy0 = 0, cx1 = v.w, cy1 = v.h;
    if (clip) { if (clip[0] > cx0) cx0 = clip[0]; if (clip[1] > cy0) cy0 = clip[1]; if (clip[2] < cx1) cx1 = clip[2]; if (clip[3] < cy1) cy1 = clip[3]; }
    uint32_t bk = colorref_rgb(d->bkcolor), fg = colorref_rgb(d->textcolor);
    if (opaque_rect && clip) fill_rect(&v, clip[0], clip[1], clip[2], clip[3], bk);
    else if (d->bkmode == 2) fill_rect(&v, x > cx0 ? x : cx0, base - asc > cy0 ? base - asc : cy0, x + w < cx1 ? x + w : cx1, base + desc < cy1 ? base + desc : cy1, bk);
    if (!f->ttf) return;
    float shear = (f->italic && !f->ttf->italic) ? 0.2f : 0;
    float bold = (f->weight >= 600 && !f->ttf->bold) ? 1.0f : 0;
    float pen = (float)x;
    for (int i = 0; i < n; i++) {
        int g = ttf_glyph_index(f->ttf, cp1252_uni((unsigned char)s[i]));
        int gw, gh, ox, oy; uint8_t *cov = ttf_render(f->ttf, g, em, shear, bold, &gw, &gh, &ox, &oy);
        int px0 = (int)lroundf(pen);
        for (int yy = 0; yy < gh && cov; yy++) {
            int Y = base + oy + yy; if (Y < cy0 || Y >= cy1) continue;
            for (int xx = 0; xx < gw; xx++) {
                int a = cov[yy * gw + xx]; if (!a) continue;
                int X = px0 + ox + xx; if (X < cx0 || X >= cx1) continue;
                uint32_t c = fg;
                if (a < 255) {
                    uint32_t o = get_rgb(&v, X, Y, NULL);
                    c = 0;
                    for (int sh = 0; sh < 24; sh += 8) { int cc = ((fg >> sh & 255) * a + (o >> sh & 255) * (255 - a)) / 255; c |= (uint32_t)cc << sh; }
                }
                put_rgb(&v, X, Y, c, -1);
            }
        }
        free(cov);
        if (dx) pen += dx[i];
        else pen += ttf_advance(f->ttf, g) * em / f->ttf->upem + bold;
    }
}
static uint32_t make_font(int h, int w, int weight, int italic, int pitch, const char *face) {
    uint32_t hf = obj_new(GO_FONT); gdiobj_t *o = obj_get(hf, GO_FONT); if (!o) return 0;
    gdi_font_t *f = &o->u.font;
    f->height = h; f->width = w; f->weight = weight; f->italic = italic; f->fixed = (pitch & 3) == 1;
    snprintf(f->face, sizeof f->face, "%s", face ? face : "");
    return hf;
}
static uint32_t WINAPI CreateFontA_(int h, int w, int e, int o, int wt, DWORD it, DWORD ul, DWORD so, DWORD cs, DWORD op, DWORD cp, DWORD q, DWORD pf, const char *face) {
    (void)e; (void)o; (void)ul; (void)so; (void)cs; (void)op; (void)cp; (void)q;
    return make_font(h, w, wt, it != 0, (int)pf, face);
}
static uint32_t WINAPI CreateFontIndirectA_(const uint8_t *lf) {
    return make_font(*(int32_t *)lf, *(int32_t *)(lf + 4), *(int32_t *)(lf + 16), lf[20] != 0, lf[27], (const char *)lf + 28);
}
static DWORD WINAPI SetTextColor_(uint32_t dc, DWORD c) { gdi_dc_t *d = gdi_get_dc(dc); if (!d) return 0xffffffff; DWORD o = d->textcolor; d->textcolor = c & 0xffffff; return o; }
static DWORD WINAPI SetBkColor_(uint32_t dc, DWORD c) { gdi_dc_t *d = gdi_get_dc(dc); if (!d) return 0xffffffff; DWORD o = d->bkcolor; d->bkcolor = c & 0xffffff; return o; }
static int WINAPI SetBkMode_(uint32_t dc, int m) { gdi_dc_t *d = gdi_get_dc(dc); if (!d) return 0; int o = d->bkmode ? d->bkmode : 2; d->bkmode = m; return o; }
static UINT WINAPI SetTextAlign_(uint32_t dc, UINT a) { gdi_dc_t *d = gdi_get_dc(dc); if (!d) return 0xffffffff; UINT o = (UINT)d->align; d->align = (int)a; return o; }
static UINT WINAPI GetTextAlign_(uint32_t dc) { gdi_dc_t *d = gdi_get_dc(dc); return d ? (UINT)d->align : 0; }
static BOOL WINAPI TextOutA_(uint32_t dc, int x, int y, const char *s, int n) { draw_text(dc, x, y, s, n, NULL, 0, NULL); return TRUE; }
static BOOL WINAPI ExtTextOutA_(uint32_t dc, int x, int y, UINT opt, const int32_t *rc, const char *s, UINT n, const int *dx) {
    draw_text(dc, x, y, s ? s : "", s ? (int)n : 0, (opt & 6) ? rc : NULL, (opt & 2) != 0, dx);
    return TRUE;
}
static BOOL WINAPI GetTextExtentPoint32A_(uint32_t dc, const char *s, int n, LONG *sz) {
    gdi_dc_t *d = gdi_get_dc(dc); gdi_font_t *f = font_of(d ? d->font : 0);
    float em; int a, de; font_metrics(f, &em, &a, &de);
    sz[0] = text_width(f, s, n, NULL); sz[1] = a + de; return TRUE;
}
static BOOL WINAPI GetTextExtentPointA_(uint32_t dc, const char *s, int n, LONG *sz) { return GetTextExtentPoint32A_(dc, s, n, sz); }
static BOOL WINAPI GetTextMetricsA_(uint32_t dc, uint8_t *tm) {
    gdi_dc_t *d = gdi_get_dc(dc); gdi_font_t *f = font_of(d ? d->font : 0);
    float em; int a, de; font_metrics(f, &em, &a, &de);
    memset(tm, 0, 56);
    int32_t *L = (int32_t *)tm;
    L[0] = a + de; L[1] = a; L[2] = de; L[3] = (int32_t)(a + de - em + 0.5f); if (L[3] < 0) L[3] = 0; L[4] = 0;
    L[5] = text_width(f, "x", 1, NULL); L[6] = text_width(f, "W", 1, NULL); L[7] = f->weight ? f->weight : 400; L[8] = 0; L[9] = L[10] = 96;
    tm[44] = 32; tm[45] = 255; tm[46] = 31; tm[47] = 32; tm[48] = (uint8_t)f->italic; tm[51] = (uint8_t)(f->fixed ? 0x30 : 0x26);
    return TRUE;
}
static BOOL WINAPI GetCharWidth32A_(uint32_t dc, UINT a, UINT b, int *out) {
    gdi_dc_t *d = gdi_get_dc(dc); gdi_font_t *f = font_of(d ? d->font : 0);
    for (UINT c = a; c <= b; c++) { char ch = (char)c; out[c - a] = text_width(f, &ch, 1, NULL); }
    return TRUE;
}
/* user32 DrawTextA (simple: one line or '\n' separated lines, alignment flags) */
int gdi_draw_text(uint32_t dc, const char *s, int n, int32_t *rc, UINT fl) {
    gdi_dc_t *d = gdi_get_dc(dc); gdi_font_t *f = font_of(d ? d->font : 0);
    float em; int a, de; font_metrics(f, &em, &a, &de);
    if (n < 0) n = (int)strlen(s);
    int lh = a + de, lines = 0, maxw = 0;
    for (int i = 0, st = 0; i <= n; i++) if (i == n || s[i] == '\n') { int w = text_width(f, s + st, i - st, NULL); if (w > maxw) maxw = w; lines++; st = i + 1; }
    if (fl & 0x400) { rc[2] = rc[0] + maxw; rc[3] = rc[1] + lines * lh; return lines * lh; }   /* DT_CALCRECT */
    int y = rc[1];
    if (fl & 0x20) { if (fl & 4) y = (rc[1] + rc[3] - lh) / 2; else if (fl & 8) y = rc[3] - lh; }
    int oldal = d ? d->align : 0; if (d) d->align = 0;
    for (int i = 0, st = 0; i <= n; i++) if (i == n || s[i] == '\n') {
        int w = text_width(f, s + st, i - st, NULL), x = rc[0];
        if (fl & 1) x = (rc[0] + rc[2] - w) / 2; else if (fl & 2) x = rc[2] - w;
        draw_text(dc, x, y, s + st, i - st, (fl & 0x100) ? NULL : rc, 0, NULL);
        y += lh; st = i + 1;
    }
    if (d) d->align = oldal;
    return lines * lh;
}

static BOOL WINAPI GdiFlush_(void) { return TRUE; }
static BOOL WINAPI PatBlt_(uint32_t dc, int x, int y, int w, int h, DWORD rop) { (void)dc; (void)x; (void)y; (void)w; (void)h; (void)rop; return TRUE; }
/* StretchDIBits onto the window: this is how the Windows ddraw.dll presents with its software renderer */
static int WINAPI StretchDIBits_(uint32_t dc, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, const void *bits, const uint8_t *bmi, UINT usage, DWORD rop) {
    (void)dx; (void)dy; (void)dw; (void)dh; (void)sx; (void)sy; (void)usage; (void)rop;
    gdi_dc_t *d = gdi_get_dc(dc);
    if (!d || !d->is_window) return 0;
    int32_t bw = *(int32_t *)(bmi + 4), bh = *(int32_t *)(bmi + 8); int bpp = *(uint16_t *)(bmi + 14);
    if (bpp != 32 || bh >= 0) { LOG("StretchDIBits: unsupported format %d bpp, h=%d", bpp, bh); return 0; }
    extern void plat_present(const uint32_t *xrgb, int w, int h, int pitch_px);
    plat_present((const uint32_t *)bits, sw, sh, bw);
    return sh;
}
static int WINAPI ChoosePixelFormat_(uint32_t dc, const void *p) { (void)dc; (void)p; return 0; }
static BOOL WINAPI SetPixelFormat_(uint32_t dc, int f, const void *p) { (void)dc; (void)f; (void)p; return FALSE; }
static BOOL WINAPI SwapBuffers_(uint32_t dc) { (void)dc; return FALSE; }
#define E(n) { "gdi32", #n, 0, (void *)n##_ }
static const export_t gdi32_exports[] = {
    E(BitBlt), E(StretchBlt), E(SetStretchBltMode), E(CreateCompatibleDC), E(DeleteDC), E(SelectObject), E(DeleteObject),
    E(GetObjectA), E(GetDIBColorTable), E(GetDeviceCaps), E(GetStockObject), E(CreateFontIndirectA), E(CreatePen),
    E(CreateRectRgn), E(SelectClipRgn), E(SetTextColor), E(SetMapMode), E(MoveToEx), E(LineTo), E(TextOutA),
    E(GetTextExtentPoint32A), E(GetTextMetricsA), E(CreateDCA), E(CreateICA), E(StartDocA), E(EndDoc), E(StartPage),
    E(EndPage), E(GetSystemPaletteUse), E(SetSystemPaletteUse), E(GetSystemPaletteEntries), E(CreateCompatibleBitmap),
    E(CreateDIBSection), E(GetDIBits), E(SetPixel), E(GetPixel), E(GdiFlush), E(PatBlt), E(StretchDIBits), E(ChoosePixelFormat),
    E(SetPixelFormat), E(SwapBuffers), E(CreateFontA), E(AddFontResourceA), E(AddFontResourceExA), E(RemoveFontResourceA), E(RemoveFontResourceExA),
    E(SetBkColor), E(SetBkMode), E(SetTextAlign), E(GetTextAlign), E(ExtTextOutA), E(GetTextExtentPointA), E(GetCharWidth32A),
    { "gdi32", "GetCharWidthA", 0, (void *)GetCharWidth32A_ },
    { NULL, NULL, 0, NULL }
};
#undef E
void gdi32_register(void) { rt_register_exports(gdi32_exports); }
