#include <stdlib.h>
/* Software rasteriser implementing the Direct3D 7 fixed-function pipeline
 * that GP3 uses. Reference implementation for tests and fallback renderer. */
#include "rt.h"
#include "rdr.h"
#include "plat.h"
#include <math.h>

struct rtex { int levels; int w[16], h[16]; uint32_t *px[16]; };
struct rtarget { int w, h; uint32_t *c; float *z; };

static rtex_t *s_tex_create(int w, int h, int levels) {
    rtex_t *t = calloc(1, sizeof *t);
    t->levels = levels > 16 ? 16 : levels;
    for (int i = 0; i < t->levels; i++) {
        t->w[i] = w; t->h[i] = h; t->px[i] = calloc((size_t)w * h, 4);
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    return t;
}
static void s_tex_upload(rtex_t *t, int l, int w, int h, const uint32_t *p) {
    if (l >= t->levels) return;
    if (t->w[l] != w || t->h[l] != h) { free(t->px[l]); t->px[l] = malloc((size_t)w * h * 4); t->w[l] = w; t->h[l] = h; }
    memcpy(t->px[l], p, (size_t)w * h * 4);
}
static void s_tex_destroy(rtex_t *t) { for (int i = 0; i < t->levels; i++) free(t->px[i]); free(t); }
static rtarget_t *s_target_create(int w, int h, int scale) {
    (void)scale;
    rtarget_t *t = calloc(1, sizeof *t);
    t->w = w; t->h = h;
    t->c = calloc((size_t)w * h, 4);
    t->z = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) t->z[i] = 1.f;
    return t;
}
static void s_target_destroy(rtarget_t *t) { free(t->c); free(t->z); free(t); }
static void s_target_upload(rtarget_t *t, const uint32_t *p, int pitch, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) if (y + j < t->h) memcpy(t->c + (size_t)(y + j) * t->w + x, p + (size_t)(y + j) * pitch + x, (size_t)w * 4);
}
static void s_target_download(rtarget_t *t, uint32_t *p, int pitch, int y0, int n) {
    for (int j = y0; j < y0 + n && j < t->h; j++) memcpy(p + (size_t)j * pitch, t->c + (size_t)j * t->w, (size_t)t->w * 4);
}
static void s_clear(rtarget_t *t, const int *rects, int n, int color, uint32_t argb, int depth, float z, int vx, int vy, int vw, int vh) {
    int one[4] = { vx, vy, vx + vw, vy + vh };
    if (!n) { rects = one; n = 1; }
    for (int i = 0; i < n; i++) {
        int x0 = rects[i * 4], y0 = rects[i * 4 + 1], x1 = rects[i * 4 + 2], y1 = rects[i * 4 + 3];
        if (vw > 0 && vh > 0) {   /* Direct3D clips clear rectangles to the viewport */
            if (x0 < vx) x0 = vx; if (y0 < vy) y0 = vy;
            if (x1 > vx + vw) x1 = vx + vw; if (y1 > vy + vh) y1 = vy + vh;
        }
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > t->w) x1 = t->w; if (y1 > t->h) y1 = t->h;
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
            if (color) t->c[y * t->w + x] = argb;
            if (depth) t->z[y * t->w + x] = z;
        }
    }
}

/* ------------------------------------------------------------------ */
typedef struct { float r, g, b, a; } col4;

static inline col4 unpack(uint32_t c) { return (col4){ ((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, (c >> 24) / 255.f }; }
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static inline int wrap_coord(int i, int n, int mode) {
    switch (mode) {
    case 2: { int p = i % (2 * n); if (p < 0) p += 2 * n; return p < n ? p : 2 * n - 1 - p; }
    case 3: case 4: return i < 0 ? 0 : i >= n ? n - 1 : i;
    default: { int p = i % n; return p < 0 ? p + n : p; }
    }
}
static col4 sample(const rstage_t *st, float u, float v, int level) {
    const rtex_t *t = st->tex;
    if (!t) return (col4){ 1, 1, 1, 1 };
    if (level >= t->levels) level = t->levels - 1;
    if (level < 0) level = 0;
    int w = t->w[level], h = t->h[level];
    const uint32_t *p = t->px[level];
    float fu = u * w - 0.5f, fv = v * h - 0.5f;
    if (st->magf == 2 || st->minf == 2) {
        int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
        float ax = fu - x0, ay = fv - y0;
        int xa = wrap_coord(x0, w, st->addru), xb = wrap_coord(x0 + 1, w, st->addru);
        int ya = wrap_coord(y0, h, st->addrv), yb = wrap_coord(y0 + 1, h, st->addrv);
        col4 c00 = unpack(p[ya * w + xa]), c10 = unpack(p[ya * w + xb]), c01 = unpack(p[yb * w + xa]), c11 = unpack(p[yb * w + xb]);
        col4 r;
        r.r = (c00.r * (1 - ax) + c10.r * ax) * (1 - ay) + (c01.r * (1 - ax) + c11.r * ax) * ay;
        r.g = (c00.g * (1 - ax) + c10.g * ax) * (1 - ay) + (c01.g * (1 - ax) + c11.g * ax) * ay;
        r.b = (c00.b * (1 - ax) + c10.b * ax) * (1 - ay) + (c01.b * (1 - ax) + c11.b * ax) * ay;
        r.a = (c00.a * (1 - ax) + c10.a * ax) * (1 - ay) + (c01.a * (1 - ax) + c11.a * ax) * ay;
        return r;
    }
    int x = wrap_coord((int)floorf(u * w), w, st->addru), y = wrap_coord((int)floorf(v * h), h, st->addrv);
    return unpack(p[y * w + x]);
}

static inline col4 get_arg(int a, col4 diff, col4 cur, col4 tex, col4 tf, col4 spec) {
    col4 r;
    switch (a & 0xf) {
    case TA_DIFFUSE: r = diff; break;
    case TA_CURRENT: r = cur; break;
    case TA_TEXTURE: r = tex; break;
    case TA_TFACTOR: r = tf; break;
    case TA_SPECULAR: r = spec; break;
    default: r = cur;
    }
    if (a & TA_COMPLEMENT) { r.r = 1 - r.r; r.g = 1 - r.g; r.b = 1 - r.b; r.a = 1 - r.a; }
    if (a & TA_ALPHAREPLICATE) { r.r = r.g = r.b = r.a; }
    return r;
}
/* one channel group (rgb or alpha) of a texture op; v1/v2 are args, extra alphas for blend ops */
static inline float op1(int op, float a1, float a2, float a1a, float diffa, float texa, float tfa, float cura) {
    switch (op) {
    case TOP_SELECTARG1: return a1;
    case TOP_SELECTARG2: return a2;
    case TOP_MODULATE: return a1 * a2;
    case TOP_MODULATE2X: return a1 * a2 * 2;
    case TOP_MODULATE4X: return a1 * a2 * 4;
    case TOP_ADD: return a1 + a2;
    case TOP_ADDSIGNED: return a1 + a2 - 0.5f;
    case TOP_ADDSIGNED2X: return (a1 + a2 - 0.5f) * 2;
    case TOP_SUBTRACT: return a1 - a2;
    case TOP_ADDSMOOTH: return a1 + a2 - a1 * a2;
    case TOP_BLENDDIFFUSEALPHA: return a1 * diffa + a2 * (1 - diffa);
    case TOP_BLENDTEXTUREALPHA: return a1 * texa + a2 * (1 - texa);
    case TOP_BLENDFACTORALPHA: return a1 * tfa + a2 * (1 - tfa);
    case TOP_BLENDTEXTUREALPHAPM: return a1 + a2 * (1 - texa);
    case TOP_BLENDCURRENTALPHA: return a1 * cura + a2 * (1 - cura);
    case TOP_MODULATEALPHA_ADDCOLOR: return a1 + a1a * a2;
    case TOP_MODULATECOLOR_ADDALPHA: return a1 * a2 + a1a;
    case TOP_MODULATEINVALPHA_ADDCOLOR: return (1 - a1a) * a2 + a1;
    case TOP_MODULATEINVCOLOR_ADDALPHA: return (1 - a1) * a2 + a1a;
    default: return a1;
    }
}

typedef struct { float x, y, z, iw; float a[14]; } sv_t; /* screen vertex; a[] = attribs * iw */
enum { A_DR, A_DG, A_DB, A_DA, A_SR, A_SG, A_SB, A_SA, A_U0, A_V0, A_U1, A_V1, A_W, A_N };

static inline int ztest(int f, float z, float zb) {
    switch (f) { case 1: return 0; case 2: return z < zb; case 3: return z == zb; case 4: return z <= zb; case 5: return z > zb;
                 case 6: return z != zb; case 7: return z >= zb; default: return 1; }
}
static inline int atest(int f, int a, int ref) {
    switch (f) { case 1: return 0; case 2: return a < ref; case 3: return a == ref; case 4: return a <= ref; case 5: return a > ref;
                 case 6: return a != ref; case 7: return a >= ref; default: return 1; }
}
static inline void blendf(int m, col4 s, col4 d, float *f) {
    switch (m) {
    case 1: f[0] = f[1] = f[2] = f[3] = 0; break;
    case 2: f[0] = f[1] = f[2] = f[3] = 1; break;
    case 3: f[0] = s.r; f[1] = s.g; f[2] = s.b; f[3] = s.a; break;
    case 4: f[0] = 1 - s.r; f[1] = 1 - s.g; f[2] = 1 - s.b; f[3] = 1 - s.a; break;
    case 5: case 12: f[0] = f[1] = f[2] = f[3] = s.a; break;
    case 6: case 13: f[0] = f[1] = f[2] = f[3] = 1 - s.a; break;
    case 7: f[0] = f[1] = f[2] = f[3] = d.a; break;
    case 8: f[0] = f[1] = f[2] = f[3] = 1 - d.a; break;
    case 9: f[0] = d.r; f[1] = d.g; f[2] = d.b; f[3] = d.a; break;
    case 10: f[0] = 1 - d.r; f[1] = 1 - d.g; f[2] = 1 - d.b; f[3] = 1 - d.a; break;
    case 11: { float x = s.a < 1 - d.a ? s.a : 1 - d.a; f[0] = f[1] = f[2] = x; f[3] = 1; break; }
    default: f[0] = f[1] = f[2] = f[3] = 1;
    }
}

int g_probe_x = -1, g_probe_y = -1, g_probe_active;
static void shade_pixel_(rtarget_t *t, const rstate_t *s, int x, int y, float z, const float *at, const int *lod);
static void shade_pixel(rtarget_t *t, const rstate_t *s, int x, int y, float z, const float *at, const int *lod) {
    if (x == g_probe_x && y == g_probe_y && g_probe_active) {
        uint32_t before = t->c[y * t->w + x];
        shade_pixel_(t, s, x, y, z, at, lod);
        uint32_t after = t->c[y * t->w + x];
        LOG("PROBE (%d,%d) %08x -> %08x z=%.4f zb=%.4f diff=%.2f,%.2f,%.2f,%.2f spec=%.2f,%.2f,%.2f,%.2f uv0=%.3f,%.3f uv1=%.3f,%.3f st=%d blend=%d %d/%d atest=%d ck=%d",
            x, y, before, after, z, t->z[y * t->w + x], at[A_DR], at[A_DG], at[A_DB], at[A_DA], at[A_SR], at[A_SG], at[A_SB], at[A_SA],
            at[A_U0], at[A_V0], at[A_U1], at[A_V1], s->nstages, s->blend, s->srcblend, s->dstblend, s->atest, s->ckey);
        for (int i = 0; i < s->nstages; i++) {
            const rstage_t *st = &s->st[i];
            LOG("   stage%d tex=%p %dx%d alpha=%d ck=%d cop=%d %x/%x aop=%d %x/%x tci=%d addr=%d/%d filt=%d/%d/%d",
                i, (void *)st->tex, st->tex ? st->tex->w[0] : 0, st->tex ? st->tex->h[0] : 0, st->has_alpha, st->has_ckey,
                st->colorop, st->carg1, st->carg2, st->alphaop, st->aarg1, st->aarg2, st->texcoord, st->addru, st->addrv, st->magf, st->minf, st->mipf);
        }
        return;
    }
    shade_pixel_(t, s, x, y, z, at, lod);
}
static void shade_pixel_(rtarget_t *t, const rstate_t *s, int x, int y, float z, const float *at, const int *lod) {
    int idx = y * t->w + x;
    if (s->zenable && !ztest(s->zfunc, z, t->z[idx])) return;
    col4 diff = { at[A_DR], at[A_DG], at[A_DB], at[A_DA] };
    col4 spec = { at[A_SR], at[A_SG], at[A_SB], at[A_SA] };
    if (s->flat) { /* flat colours are passed in constant attributes already */ }
    col4 tf = unpack(s->tfactor);
    col4 cur = diff;
    for (int i = 0; i < s->nstages; i++) {
        const rstage_t *st = &s->st[i];
        float u = at[st->texcoord ? A_U1 : A_U0], v = at[st->texcoord ? A_V1 : A_V0];
        col4 tex = sample(st, u, v, lod[i]);
        if (st->tex && !st->has_alpha) tex.a = 1;
        if (st->tex && st->has_ckey && s->ckey && tex.a < 0.5f) return;   /* colour key */
        if (st->tex && st->has_ckey && !(st->has_alpha && !st->has_ckey) && !s->ckey) tex.a = 1;
        col4 a1 = get_arg(st->carg1, diff, cur, tex, tf, spec), a2 = get_arg(st->carg2, diff, cur, tex, tf, spec);
        col4 b1 = get_arg(st->aarg1, diff, cur, tex, tf, spec), b2 = get_arg(st->aarg2, diff, cur, tex, tf, spec);
        col4 n = cur;
        if (st->colorop == TOP_DOTPRODUCT3) {
            float d = 4 * ((a1.r - .5f) * (a2.r - .5f) + (a1.g - .5f) * (a2.g - .5f) + (a1.b - .5f) * (a2.b - .5f));
            n.r = n.g = n.b = n.a = clamp01(d);
        } else if (st->colorop != TOP_DISABLE) {
            n.r = clamp01(op1(st->colorop, a1.r, a2.r, a1.a, diff.a, tex.a, tf.a, cur.a));
            n.g = clamp01(op1(st->colorop, a1.g, a2.g, a1.a, diff.a, tex.a, tf.a, cur.a));
            n.b = clamp01(op1(st->colorop, a1.b, a2.b, a1.a, diff.a, tex.a, tf.a, cur.a));
        }
        if (st->alphaop != TOP_DISABLE && st->colorop != TOP_DOTPRODUCT3)
            n.a = clamp01(op1(st->alphaop, b1.a, b2.a, b1.a, diff.a, tex.a, tf.a, cur.a));
        cur = n;
    }
    if (s->specular) { cur.r = clamp01(cur.r + spec.r); cur.g = clamp01(cur.g + spec.g); cur.b = clamp01(cur.b + spec.b); }
    if (s->atest && !atest(s->afunc, (int)(cur.a * 255.f + 0.5f), (int)s->aref)) return;
    if (s->fog) {
        float f;
        if (s->fogtable) {
            float d = s->fogend > 1.5f ? at[A_W] : z;
            if (s->fogtable == 3) f = (s->fogend - d) / (s->fogend - s->fogstart != 0 ? s->fogend - s->fogstart : 1);
            else if (s->fogtable == 1) f = expf(-d * s->fogdensity);
            else f = expf(-(d * s->fogdensity) * (d * s->fogdensity));
        } else f = spec.a;
        f = clamp01(f);
        col4 fc = unpack(s->fogcolor);
        cur.r = cur.r * f + fc.r * (1 - f); cur.g = cur.g * f + fc.g * (1 - f); cur.b = cur.b * f + fc.b * (1 - f);
    }
    uint32_t *dst = &t->c[idx];
    if (s->blend) {
        col4 d = unpack(*dst);
        float fs[4], fd[4];
        blendf(s->srcblend, cur, d, fs);
        if (s->srcblend == 12) { fd[0] = fd[1] = fd[2] = fd[3] = 1 - cur.a; } else if (s->srcblend == 13) { fd[0] = fd[1] = fd[2] = fd[3] = cur.a; }
        else blendf(s->dstblend, cur, d, fd);
        cur.r = clamp01(cur.r * fs[0] + d.r * fd[0]);
        cur.g = clamp01(cur.g * fs[1] + d.g * fd[1]);
        cur.b = clamp01(cur.b * fs[2] + d.b * fd[2]);
        cur.a = clamp01(cur.a * fs[3] + d.a * fd[3]);
    }
    *dst = (uint32_t)(cur.a * 255.f + .5f) << 24 | (uint32_t)(cur.r * 255.f + .5f) << 16 | (uint32_t)(cur.g * 255.f + .5f) << 8 | (uint32_t)(cur.b * 255.f + .5f);
    if (s->zenable && s->zwrite) t->z[idx] = z;
}

static void to_screen(rtarget_t *t, const rvert_t *v, sv_t *o, const rvert_t *flatsrc, int flat) {
    float iw = 1.f / v->w;
    o->iw = iw;
    o->x = (v->x * iw * 0.5f + 0.5f) * t->w;
    o->y = (0.5f - v->y * iw * 0.5f) * t->h;
    o->z = v->z * iw;
    const rvert_t *c = flat ? flatsrc : v;
    uint32_t d = c->diffuse, s = c->specular;
    float a[A_N] = { ((d >> 16) & 255) / 255.f, ((d >> 8) & 255) / 255.f, (d & 255) / 255.f, (d >> 24) / 255.f,
                     ((s >> 16) & 255) / 255.f, ((s >> 8) & 255) / 255.f, (s & 255) / 255.f, (v->specular >> 24) / 255.f,
                     v->uv[0][0], v->uv[0][1], v->uv[1][0], v->uv[1][1], v->w };
    for (int i = 0; i < A_N; i++) o->a[i] = a[i] * iw;
}

static int tri_lod(const rstate_t *s, int st, const sv_t *v[3]) {
    const rstage_t *S = &s->st[st];
    if (!S->tex || S->mipf <= 1 || S->tex->levels <= 1) return 0;
    int ui = S->texcoord ? A_U1 : A_U0, vi = S->texcoord ? A_V1 : A_V0;
    float u[3], vv[3];
    for (int i = 0; i < 3; i++) { u[i] = v[i]->a[ui] / v[i]->iw * S->tex->w[0]; vv[i] = v[i]->a[vi] / v[i]->iw * S->tex->h[0]; }
    float ta = fabsf((u[1] - u[0]) * (vv[2] - vv[0]) - (u[2] - u[0]) * (vv[1] - vv[0]));
    float pa = fabsf((v[1]->x - v[0]->x) * (v[2]->y - v[0]->y) - (v[2]->x - v[0]->x) * (v[1]->y - v[0]->y));
    if (pa < 1e-6f) return 0;
    float r = ta / pa;
    int l = r > 1 ? (int)(0.5f * log2f(r) + 0.5f + S->lodbias) : 0;
    return l < 0 ? 0 : l;
}

static void raster_tri(rtarget_t *t, const rstate_t *s, const sv_t *v0, const sv_t *v1, const sv_t *v2) {
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0 || !isfinite(area)) return;
    if (s->cull == 3 && area < 0) return;   /* cull counter-clockwise */
    if (s->cull == 2 && area > 0) return;   /* cull clockwise */
    if (area < 0) { const sv_t *tmp = v1; v1 = v2; v2 = tmp; area = -area; }
    int minx = (int)floorf(fminf(v0->x, fminf(v1->x, v2->x))), maxx = (int)ceilf(fmaxf(v0->x, fmaxf(v1->x, v2->x)));
    int miny = (int)floorf(fminf(v0->y, fminf(v1->y, v2->y))), maxy = (int)ceilf(fmaxf(v0->y, fmaxf(v1->y, v2->y)));
    int cx0 = s->vw ? s->vx : 0, cy0 = s->vw ? s->vy : 0, cx1 = s->vw ? s->vx + s->vw : t->w, cy1 = s->vh ? s->vy + s->vh : t->h;
    if (cx0 < 0) cx0 = 0; if (cy0 < 0) cy0 = 0; if (cx1 > t->w) cx1 = t->w; if (cy1 > t->h) cy1 = t->h;
    if (minx < cx0) minx = cx0; if (miny < cy0) miny = cy0; if (maxx > cx1) maxx = cx1; if (maxy > cy1) maxy = cy1;
    if (minx >= maxx || miny >= maxy) return;
    const sv_t *vs[3] = { v0, v1, v2 };
    int lod[2] = { 0, 0 };
    for (int i = 0; i < s->nstages; i++) lod[i] = tri_lod(s, i, vs);
    float inv = 1.f / area;
    float zb = s->zbias * (1.f / 65536.f);
    for (int y = miny; y < maxy; y++) {
        float py = y + 0.5f;
        for (int x = minx; x < maxx; x++) {
            float px = x + 0.5f;
            float w0 = (v2->x - v1->x) * (py - v1->y) - (v2->y - v1->y) * (px - v1->x);
            float w1 = (v0->x - v2->x) * (py - v2->y) - (v0->y - v2->y) * (px - v2->x);
            float w2 = (v1->x - v0->x) * (py - v0->y) - (v1->y - v0->y) * (px - v0->x);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            /* top-left rule approximated: exclude pixels exactly on right/bottom edges */
            if ((w0 == 0 && (v2->y - v1->y) > 0) || (w1 == 0 && (v0->y - v2->y) > 0) || (w2 == 0 && (v1->y - v0->y) > 0)) continue;
            w0 *= inv; w1 *= inv; w2 *= inv;
            float z = w0 * v0->z + w1 * v1->z + w2 * v2->z - zb;
            float iw = w0 * v0->iw + w1 * v1->iw + w2 * v2->iw;
            float rw = 1.f / iw;
            float at[A_N];
            for (int k = 0; k < A_N; k++) at[k] = (w0 * v0->a[k] + w1 * v1->a[k] + w2 * v2->a[k]) * rw;
            shade_pixel(t, s, x, y, z, at, lod);
        }
    }
}

static void raster_line(rtarget_t *t, const rstate_t *s, const sv_t *a, const sv_t *b) {
    float dx = b->x - a->x, dy = b->y - a->y;
    int n = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)));
    if (n <= 0) n = 1;
    int lod[2] = { 0, 0 };
    for (int i = 0; i <= n; i++) {
        float f = (float)i / n;
        int x = (int)floorf(a->x + dx * f), y = (int)floorf(a->y + dy * f);
        if (x < 0 || y < 0 || x >= t->w || y >= t->h) continue;
        float iw = a->iw + (b->iw - a->iw) * f, rw = 1.f / iw;
        float at[A_N];
        for (int k = 0; k < A_N; k++) at[k] = (a->a[k] + (b->a[k] - a->a[k]) * f) * rw;
        shade_pixel(t, s, x, y, a->z + (b->z - a->z) * f, at, lod);
    }
}

/* clip a triangle against w > epsilon (only matters for untransformed geometry) */
static int clip_near(const rvert_t *in, rvert_t *out) {
    const float eps = 1e-5f; int n = 0;
    for (int i = 0; i < 3; i++) {
        const rvert_t *a = &in[i], *b = &in[(i + 1) % 3];
        int ia = a->w > eps, ib = b->w > eps;
        if (ia) out[n++] = *a;
        if (ia != ib) {
            float f = (eps - a->w) / (b->w - a->w);
            rvert_t v = *a;
            v.x = a->x + (b->x - a->x) * f; v.y = a->y + (b->y - a->y) * f; v.z = a->z + (b->z - a->z) * f; v.w = eps;
            for (int k = 0; k < 2; k++) { v.uv[k][0] = a->uv[k][0] + (b->uv[k][0] - a->uv[k][0]) * f; v.uv[k][1] = a->uv[k][1] + (b->uv[k][1] - a->uv[k][1]) * f; }
            out[n++] = v;
        }
    }
    return n;
}

static void s_draw(rtarget_t *t, const rstate_t *s, int prim, const rvert_t *v, int nv, const uint16_t *idx, int ni) {
    { static int nd = -1; if (nd < 0) { extern char *getenv(const char *); nd = getenv("GP3_NULL_DRAW") != 0; } if (nd) return; }   /* testing: measure the game without rasterising */
    { static int nul = -1; if (nul < 0) nul = getenv("GP3_NULL_DRAW") != NULL; if (nul) return; }
    (void)nv;
    if (prim == RPRIM_TRIS) {
        for (int i = 0; i + 2 < ni; i += 3) {
            rvert_t tri[3] = { v[idx[i]], v[idx[i + 1]], v[idx[i + 2]] };
            rvert_t poly[4];
            int n = (tri[0].w > 1e-5f && tri[1].w > 1e-5f && tri[2].w > 1e-5f) ? (memcpy(poly, tri, sizeof tri), 3) : clip_near(tri, poly);
            sv_t sv[4];
            for (int k = 0; k < n; k++) to_screen(t, &poly[k], &sv[k], &tri[0], s->flat);
            for (int k = 1; k + 1 < n; k++) raster_tri(t, s, &sv[0], &sv[k], &sv[k + 1]);
        }
    } else if (prim == RPRIM_LINES) {
        for (int i = 0; i + 1 < ni; i += 2) {
            sv_t a, b; to_screen(t, &v[idx[i]], &a, &v[idx[i]], s->flat); to_screen(t, &v[idx[i + 1]], &b, &v[idx[i]], s->flat);
            raster_line(t, s, &a, &b);
        }
    } else if (prim == RPRIM_POINTS) {
        int lod[2] = { 0, 0 };
        for (int i = 0; i < ni; i++) {
            sv_t a; to_screen(t, &v[idx[i]], &a, &v[idx[i]], 0);
            int x = (int)floorf(a.x), y = (int)floorf(a.y);
            if (x < 0 || y < 0 || x >= t->w || y >= t->h) continue;
            float at[A_N]; for (int k = 0; k < A_N; k++) at[k] = a.a[k] / a.iw;
            shade_pixel(t, s, x, y, a.z, at, lod);
        }
    }
}

static void s_blit(rtarget_t *t, int dx, int dy, int dw, int dh, const uint32_t *p, int sw, int sh, int pitch, int keyed) {
    for (int y = 0; y < dh; y++) {
        int Y = dy + y; if (Y < 0 || Y >= t->h) continue;
        int sy = y * sh / dh;
        for (int x = 0; x < dw; x++) {
            int X = dx + x; if (X < 0 || X >= t->w) continue;
            uint32_t c = p[sy * pitch + x * sw / dw];
            if (keyed && !(c >> 24)) continue;
            t->c[Y * t->w + X] = c;
        }
    }
}
static void s_copy_from(rtarget_t *t, int dx, int dy, int dw, int dh, rtarget_t *src, int sx, int sy, int sw, int sh) {
    uint32_t *tmp = malloc((size_t)sw * sh * 4);
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) {
        int X = sx + x, Y = sy + y;
        tmp[y * sw + x] = (X >= 0 && Y >= 0 && X < src->w && Y < src->h) ? src->c[Y * src->w + X] : 0;
    }
    s_blit(t, dx, dy, dw, dh, tmp, sw, sh, sw, 0);
    free(tmp);
}
static void s_copy(rtarget_t *t, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
    uint32_t *tmp = malloc((size_t)sw * sh * 4);
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) {
        int X = sx + x, Y = sy + y;
        tmp[y * sw + x] = (X >= 0 && Y >= 0 && X < t->w && Y < t->h) ? t->c[Y * t->w + X] : 0;
    }
    s_blit(t, dx, dy, dw, dh, tmp, sw, sh, sw, 0);
    free(tmp);
}
static void s_present(rtarget_t *t, int w, int h) { (void)w; (void)h; plat_present(t->c, t->w, t->h, t->w); }
static void s_present_argb(const uint32_t *p, int w, int h, int pitch) { plat_present(p, w, h, pitch); }

static rbackend_t g_soft = {
    "software", s_tex_create, s_tex_upload, s_tex_destroy, s_target_create, s_target_destroy, s_target_upload,
    s_target_download, s_clear, s_draw, s_blit, s_copy, s_present, s_present_argb, s_copy_from
};
rbackend_t *rdr_soft_backend(void) { return &g_soft; }
