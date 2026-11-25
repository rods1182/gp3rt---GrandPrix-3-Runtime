/* Small TrueType renderer for the GDI text functions (GPxPatch draws its TV graphics
 * text with fonts rendered through GDI). Reads cmap (formats 4 and 12), hmtx, loca/glyf
 * (simple and composite glyphs) and fills outlines with an exact-area accumulation
 * rasteriser (anti-aliased coverage, as in font-rs / stb_truetype v2). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "ttf.h"

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static int16_t bes16(const uint8_t *p) { return (int16_t)be16(p); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint32_t find_table(const uint8_t *d, size_t n, const char *tag) {
    if (n < 12) return 0;
    int nt = be16(d + 4);
    for (int i = 0; i < nt; i++) {
        const uint8_t *r = d + 12 + 16 * i;
        if ((size_t)(12 + 16 * i + 16) > n) break;
        if (!memcmp(r, tag, 4)) return be32(r + 8);
    }
    return 0;
}

ttf_t *ttf_load(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 64 || n > 64 * 1024 * 1024) { fclose(f); return NULL; }
    uint8_t *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return NULL; } fclose(f);
    uint32_t v = be32(d);
    if (v == 0x74746366) { /* ttc: first font */ uint32_t off = be32(d + 12); memmove(d, d + off, n - off); n -= off; }
    ttf_t *t = calloc(1, sizeof *t); t->data = d; t->size = (size_t)n;
    uint32_t head = find_table(d, n, "head"), hhea = find_table(d, n, "hhea"), maxp = find_table(d, n, "maxp");
    t->loca = find_table(d, n, "loca"); t->glyf = find_table(d, n, "glyf"); t->hmtx = find_table(d, n, "hmtx");
    uint32_t cmap = find_table(d, n, "cmap"), name = find_table(d, n, "name"), os2 = find_table(d, n, "OS/2");
    if (!head || !hhea || !maxp || !t->loca || !t->glyf || !t->hmtx || !cmap) { free(d); free(t); return NULL; }
    t->upem = be16(d + head + 18); t->loca_long = bes16(d + head + 50);
    t->ascent = bes16(d + hhea + 4); t->descent = bes16(d + hhea + 6); t->linegap = bes16(d + hhea + 8);
    t->nhmetrics = be16(d + hhea + 34); t->nglyphs = be16(d + maxp + 4);
    if (os2 && be16(d + os2 + 4) >= 600) t->bold = 1;
    if (os2 && (be16(d + os2 + 62) & 1)) t->italic = 1;   /* fsSelection ITALIC */
    if (head && (be16(d + head + 44) & 1)) t->bold = 1;   /* macStyle bold */
    if (head && (be16(d + head + 44) & 2)) t->italic = 1;
    if (os2) { int16_t wa = bes16(d + os2 + 74), wd = bes16(d + os2 + 76); if (wa > 0) { t->win_ascent = (uint16_t)wa; t->win_descent = (uint16_t)wd; } }
    if (!t->win_ascent) { t->win_ascent = (uint16_t)t->ascent; t->win_descent = (uint16_t)-t->descent; }
    /* cmap: prefer (3,10) format 12, then (3,1)/(0,x) format 4 */
    int nsub = be16(d + cmap + 2);
    for (int pass = 0; pass < 2 && !t->cmap_sub; pass++)
        for (int i = 0; i < nsub; i++) {
            const uint8_t *r = d + cmap + 4 + 8 * i;
            int pid = be16(r), eid = be16(r + 2); uint32_t off = cmap + be32(r + 4);
            int fmt = be16(d + off);
            if (pass == 0 && fmt == 12 && (pid == 3 || pid == 0)) { t->cmap_sub = off; t->cmap_fmt = 12; break; }
            if (pass == 1 && fmt == 4 && (pid == 3 || pid == 0)) { t->cmap_sub = off; t->cmap_fmt = 4; t->symbol = pid == 3 && eid == 0; break; }
        }
    /* family name (name id 1), any platform, ASCII-ised */
    if (name) {
        int cnt = be16(d + name + 2); uint32_t so = name + be16(d + name + 4);
        for (int i = 0; i < cnt; i++) {
            const uint8_t *r = d + name + 6 + 12 * i;
            int pid = be16(r), nid = be16(r + 6), len = be16(r + 8), off = be16(r + 10);
            if (nid != 1) continue;
            char tmp[64]; int k = 0;
            if (pid == 3 || pid == 0) for (int j = 0; j + 1 < len && k < 63; j += 2) tmp[k++] = (char)d[so + off + j + 1];
            else for (int j = 0; j < len && k < 63; j++) tmp[k++] = (char)d[so + off + j];
            tmp[k] = 0;
            if (k) { snprintf(t->family, sizeof t->family, "%s", tmp); if (pid == 3) break; }
        }
    }
    return t;
}

int ttf_glyph_index(const ttf_t *t, uint32_t cp) {
    const uint8_t *d = t->data;
    if (!t->cmap_sub) return 0;
    if (t->symbol && cp < 0x100) cp += 0xf000;
    if (t->cmap_fmt == 12) {
        uint32_t ng = be32(d + t->cmap_sub + 12);
        for (uint32_t i = 0; i < ng; i++) {
            const uint8_t *g = d + t->cmap_sub + 16 + 12 * i;
            uint32_t s = be32(g), e = be32(g + 4);
            if (cp >= s && cp <= e) return (int)(be32(g + 8) + cp - s);
        }
        return 0;
    }
    if (cp > 0xffff) return 0;
    uint32_t o = t->cmap_sub; int segx2 = be16(d + o + 6);
    uint32_t ends = o + 14, starts = ends + segx2 + 2, deltas = starts + segx2, ranges = deltas + segx2;
    for (int i = 0; i < segx2 / 2; i++) {
        uint16_t e = be16(d + ends + 2 * i);
        if (cp > e) continue;
        uint16_t s = be16(d + starts + 2 * i);
        if (cp < s) return 0;
        uint16_t delta = be16(d + deltas + 2 * i), ro = be16(d + ranges + 2 * i);
        if (!ro) return (uint16_t)(cp + delta);
        uint32_t ga = ranges + 2 * i + ro + 2 * (cp - s);
        uint16_t g = be16(d + ga);
        return g ? (uint16_t)(g + delta) : 0;
    }
    return 0;
}

int ttf_advance(const ttf_t *t, int g) {
    if (g < 0 || g >= t->nglyphs) return 0;
    int i = g < t->nhmetrics ? g : t->nhmetrics - 1;
    return be16(t->data + t->hmtx + 4 * i);
}

static uint32_t glyph_offset(const ttf_t *t, int g, uint32_t *len) {
    if (g < 0 || g >= t->nglyphs) { *len = 0; return 0; }
    uint32_t a, b;
    if (t->loca_long) { a = be32(t->data + t->loca + 4 * g); b = be32(t->data + t->loca + 4 * g + 4); }
    else { a = be16(t->data + t->loca + 2 * g) * 2u; b = be16(t->data + t->loca + 2 * g + 2) * 2u; }
    *len = b > a ? b - a : 0;
    return t->glyf + a;
}

/* ---- outline -> line segments (font units, then transformed) ---- */
typedef struct { float x0, y0, x1, y1; } seg_t;
typedef struct { seg_t *s; int n, cap; } segs_t;
static void seg_add(segs_t *S, float x0, float y0, float x1, float y1) {
    if (S->n == S->cap) { S->cap = S->cap ? S->cap * 2 : 256; S->s = realloc(S->s, S->cap * sizeof *S->s); }
    S->s[S->n++] = (seg_t){ x0, y0, x1, y1 };
}
typedef struct { float a, b, c, d, e, f; } xf_t;   /* x' = a x + c y + e ; y' = b x + d y + f */
static void quad(segs_t *S, float x0, float y0, float cx, float cy, float x1, float y1) {
    float dd = fabsf(x0 - 2 * cx + x1) + fabsf(y0 - 2 * cy + y1);
    int n = 1 + (int)sqrtf(dd * 4); if (n > 32) n = 32;
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        float x = u * u * x0 + 2 * u * t * cx + t * t * x1, y = u * u * y0 + 2 * u * t * cy + t * t * y1;
        seg_add(S, px, py, x, y); px = x; py = y;
    }
}
static void outline(const ttf_t *t, int g, xf_t m, segs_t *S, int depth) {
    uint32_t len; uint32_t o = glyph_offset(t, g, &len);
    if (!len || depth > 8) return;
    const uint8_t *d = t->data;
    int nc = bes16(d + o);
    if (nc >= 0) {
        const uint8_t *ends = d + o + 10;
        int npts = nc ? be16(ends + 2 * (nc - 1)) + 1 : 0;
        int il = be16(ends + 2 * nc);
        const uint8_t *p = ends + 2 * nc + 2 + il;
        uint8_t *fl = malloc(npts + 1); float *xs = malloc(sizeof(float) * (npts + 1)), *ys = malloc(sizeof(float) * (npts + 1));
        for (int i = 0; i < npts;) { uint8_t f = *p++; fl[i++] = f; if (f & 8) { int r = *p++; while (r-- > 0 && i < npts) fl[i++] = f; } }
        int v = 0;
        for (int i = 0; i < npts; i++) { uint8_t f = fl[i]; if (f & 2) { int dx = *p++; v += (f & 16) ? dx : -dx; } else if (!(f & 16)) { v += bes16(p); p += 2; } xs[i] = (float)v; }
        v = 0;
        for (int i = 0; i < npts; i++) { uint8_t f = fl[i]; if (f & 4) { int dy = *p++; v += (f & 32) ? dy : -dy; } else if (!(f & 32)) { v += bes16(p); p += 2; } ys[i] = (float)v; }
        for (int i = 0; i < npts; i++) { float x = xs[i], y = ys[i]; xs[i] = m.a * x + m.c * y + m.e; ys[i] = m.b * x + m.d * y + m.f; }
        int s = 0;
        for (int c = 0; c < nc; c++) {
            int e = be16(ends + 2 * c), cnt = e - s + 1;
            if (cnt < 2) { s = e + 1; continue; }
            /* start at an on-curve point (or the midpoint of two off-curve ones) */
            int st = -1; for (int k = 0; k < cnt; k++) if (fl[s + k] & 1) { st = k; break; }
            float sx, sy;
            if (st < 0) { sx = (xs[s] + xs[s + 1]) / 2; sy = (ys[s] + ys[s + 1]) / 2; st = 0; }
            else { sx = xs[s + st]; sy = ys[s + st]; }
            float cx = sx, cy = sy; int have_ctrl = 0; float qx = 0, qy = 0;
            for (int k = 1; k <= cnt; k++) {
                int i = s + (st + k) % cnt;
                float x = xs[i], y = ys[i];
                if (fl[i] & 1) {
                    if (have_ctrl) quad(S, cx, cy, qx, qy, x, y); else seg_add(S, cx, cy, x, y);
                    cx = x; cy = y; have_ctrl = 0;
                } else {
                    if (have_ctrl) { float mx = (qx + x) / 2, my = (qy + y) / 2; quad(S, cx, cy, qx, qy, mx, my); cx = mx; cy = my; }
                    qx = x; qy = y; have_ctrl = 1;
                }
            }
            if (have_ctrl) quad(S, cx, cy, qx, qy, sx, sy); else if (cx != sx || cy != sy) seg_add(S, cx, cy, sx, sy);
            s = e + 1;
        }
        free(fl); free(xs); free(ys);
    } else {
        const uint8_t *p = d + o + 10;
        for (;;) {
            uint16_t fl = be16(p), gi = be16(p + 2); p += 4;
            float dx, dy;
            if (fl & 1) { dx = bes16(p); dy = bes16(p + 2); p += 4; } else { dx = (int8_t)p[0]; dy = (int8_t)p[1]; p += 2; }
            float a = 1, b = 0, c = 0, dd = 1;
            if (fl & 8) { a = dd = bes16(p) / 16384.f; p += 2; }
            else if (fl & 0x40) { a = bes16(p) / 16384.f; dd = bes16(p + 2) / 16384.f; p += 4; }
            else if (fl & 0x80) { a = bes16(p) / 16384.f; b = bes16(p + 2) / 16384.f; c = bes16(p + 4) / 16384.f; dd = bes16(p + 6) / 16384.f; p += 8; }
            /* child: first apply its own transform + offset (args as x/y offsets), then ours */
            xf_t cm = { m.a * a + m.c * b, m.b * a + m.d * b, m.a * c + m.c * dd, m.b * c + m.d * dd,
                        m.a * dx + m.c * dy + m.e, m.b * dx + m.d * dy + m.f };
            outline(t, gi, cm, S, depth + 1);
            if (!(fl & 0x20)) break;
        }
    }
}

/* accumulate signed area of each line into a (w+1) x h float buffer (font-rs algorithm;
   the caller pads the box so every x lies inside [1, w-2]) */
static void draw_line(float *acc, int w, int h, float x0, float y0, float x1, float y1) {
    if (fabsf(y0 - y1) <= 1e-6f) return;
    float dir = 1;
    if (y0 > y1) { dir = -1; float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    int ys = y0 < 0 ? 0 : (int)y0;
    if (y0 < 0) x -= y0 * dxdy;
    int ye = (int)ceilf(y1); if (ye > h) ye = h;
    for (int y = ys; y < ye; y++) {
        float *row = acc + (size_t)y * (w + 1);
        float dy = fminf((float)(y + 1), y1) - fmaxf((float)y, y0);
        float xnext = x + dxdy * dy, d = dy * dir;
        float xa = x < xnext ? x : xnext, xb = x < xnext ? xnext : x;
        float xaf = floorf(xa); int xai = (int)xaf;
        float xbc = ceilf(xb); int xbi = (int)xbc;
        if (xai < 0 || xbi > w) { x = xnext; continue; }
        if (xbi <= xai + 1) {
            float xmf = 0.5f * (x + xnext) - xaf;
            row[xai] += d - d * xmf; row[xai + 1] += d * xmf;
        } else {
            float sr = 1.0f / (xb - xa), x0f = xa - xaf;
            float a0 = 0.5f * sr * (1 - x0f) * (1 - x0f);
            float x1f = xb - xbc + 1, am = 0.5f * sr * x1f * x1f;
            row[xai] += d * a0;
            if (xbi == xai + 2) row[xai + 1] += d * (1 - a0 - am);
            else {
                float a1 = sr * (1.5f - x0f);
                row[xai + 1] += d * (a1 - a0);
                for (int xi = xai + 2; xi < xbi - 1; xi++) row[xi] += d * sr;
                float a2 = a1 + (xbi - xai - 3) * sr;
                row[xbi - 1] += d * (1 - a2 - am);
            }
            row[xbi] += d * am;
        }
        x = xnext;
    }
}

/* renders glyph g at pixel size `px` (em height); returns 8-bit coverage bitmap */
uint8_t *ttf_render(const ttf_t *t, int g, float px, float shear, float embolden, int *w, int *h, int *ox, int *oy) {
    float sc = px / t->upem;
    segs_t S = { 0 };
    xf_t m = { sc, 0, sc * shear, -sc, 0, 0 };   /* y down */
    outline(t, g, m, &S, 0);
    if (!S.n) { *w = *h = 0; *ox = *oy = 0; return NULL; }
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < S.n; i++) {
        seg_t s = S.s[i];
        minx = fminf(minx, fminf(s.x0, s.x1)); maxx = fmaxf(maxx, fmaxf(s.x0, s.x1));
        miny = fminf(miny, fminf(s.y0, s.y1)); maxy = fmaxf(maxy, fmaxf(s.y0, s.y1));
    }
    int bx = (int)floorf(minx) - 1, by = (int)floorf(miny), bw = (int)ceilf(maxx) - bx + 2 + (int)ceilf(embolden), bh = (int)ceilf(maxy) - by + 1;
    if (bw <= 0 || bh <= 0 || bw > 4096 || bh > 4096) { free(S.s); *w = *h = 0; return NULL; }
    float *acc = calloc((size_t)(bw + 1) * bh, sizeof(float));
    for (int i = 0; i < S.n; i++) draw_line(acc, bw, bh, S.s[i].x0 - bx, S.s[i].y0 - by, S.s[i].x1 - bx, S.s[i].y1 - by);
    uint8_t *out = calloc((size_t)bw * bh, 1);
    for (int y = 0; y < bh; y++) {
        float a = 0; float *row = acc + (size_t)y * (bw + 1);
        for (int x = 0; x < bw; x++) { a += row[x]; float v = fabsf(a); if (v > 1) v = 1; out[(size_t)y * bw + x] = (uint8_t)(v * 255 + 0.5f); }
    }
    if (embolden > 0) {   /* synthetic bold: smear horizontally */
        int e = (int)ceilf(embolden);
        for (int y = 0; y < bh; y++) for (int x = bw - 1; x >= 0; x--) { int m2 = out[(size_t)y * bw + x]; for (int k = 1; k <= e && x - k >= 0; k++) if (out[(size_t)y * bw + x - k] > m2) m2 = out[(size_t)y * bw + x - k]; out[(size_t)y * bw + x] = (uint8_t)m2; }
    }
    free(acc); free(S.s);
    *w = bw; *h = bh; *ox = bx; *oy = by;
    return out;
}
