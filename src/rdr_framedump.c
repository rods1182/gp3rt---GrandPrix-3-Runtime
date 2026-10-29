/* Frame dump (diagnostics): a wrapper backend that, when armed (Ctrl+Print
 * Screen), writes a description of every renderer call of the next frame to
 * <dir>/frame.txt plus the textures of large draws as PNG, so a visual glitch
 * reported by a player can be traced to the exact draw call.
 * It keeps a CPU copy of level 0 of every texture while enabled. */
#include "rdr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern int write_png_rgba(const char *path, const uint32_t *argb, int w, int h);
extern int g_spray_opacity;
void rt_log(int lvl, const char *fmt, ...);
int plat_mkdir(const char *path);

typedef struct { rtex_t *real; int w, h, id, dumped, levels, lvl_seen; uint32_t *px; } ftex_t;
typedef struct { rtarget_t *real; int w, h, id; } ftgt_t;
static rbackend_t *g_in;
static volatile int g_armed;     /* 1 = record the next frame */
static int g_recording;
static FILE *g_ft;
static char g_dir[600];
static int g_next_tex = 1, g_next_tgt = 1, g_ndraw;
static int g_count;
static char g_base[600];

void rdr_framedump_arm(void) { if (g_in && !g_armed && !g_recording) g_armed = 1; }

static rtex_t *f_tex_create(int w, int h, int l) {
    ftex_t *t = calloc(1, sizeof *t); t->real = g_in->tex_create(w, h, l); t->w = w; t->h = h; t->id = g_next_tex++; t->levels = l;
    return (rtex_t *)t;
}
static void f_tex_upload(rtex_t *tt, int level, int w, int h, const uint32_t *p) {
    ftex_t *t = (ftex_t *)tt;
    if (level < 31) t->lvl_seen |= 1 << level;
    if (level == 0) { if (!t->px || w != t->w || h != t->h) { free(t->px); t->px = malloc((size_t)w * h * 4); t->w = w; t->h = h; } if (t->px) memcpy(t->px, p, (size_t)w * h * 4); t->dumped = 0; }
    g_in->tex_upload(t->real, level, w, h, p);
}
static void f_tex_destroy(rtex_t *tt) { ftex_t *t = (ftex_t *)tt; g_in->tex_destroy(t->real); free(t->px); free(t); }
static rtarget_t *f_target_create(int w, int h, int s) { ftgt_t *t = calloc(1, sizeof *t); t->real = g_in->target_create(w, h, s); t->w = w; t->h = h; t->id = g_next_tgt++; return (rtarget_t *)t; }
static void f_target_destroy(rtarget_t *tt) { ftgt_t *t = (ftgt_t *)tt; g_in->target_destroy(t->real); free(t); }
#define T(x) (((ftgt_t *)(x))->real)
static void f_target_upload(rtarget_t *t, const uint32_t *p, int pitch, int x, int y, int w, int h) {
    if (g_recording) fprintf(g_ft, "UPLOAD target#%d rect %d,%d %dx%d\n", ((ftgt_t *)t)->id, x, y, w, h);
    g_in->target_upload(T(t), p, pitch, x, y, w, h);
}
static void f_target_download(rtarget_t *t, uint32_t *p, int pitch, int y0, int n) {
    if (g_recording) fprintf(g_ft, "DOWNLOAD target#%d rows %d..%d\n", ((ftgt_t *)t)->id, y0, y0 + n - 1);
    g_in->target_download(T(t), p, pitch, y0, n);
}
static void f_clear(rtarget_t *t, const int *r, int n, int c, uint32_t argb, int d, float z, int vx, int vy, int vw, int vh) {
    if (g_recording) {
        fprintf(g_ft, "CLEAR target#%d color=%d argb=%08x depth=%d z=%.3f rects=%d vp=%d,%d %dx%d", ((ftgt_t *)t)->id, c, argb, d, z, n, vx, vy, vw, vh);
        for (int i = 0; i < n && i < 8; i++) fprintf(g_ft, " [%d,%d-%d,%d]", r[i * 4], r[i * 4 + 1], r[i * 4 + 2], r[i * 4 + 3]);
        fputc('\n', g_ft);
    }
    g_in->clear(T(t), r, n, c, argb, d, z, vx, vy, vw, vh);
}
static int g_frame_no;
static void dump_tex(ftex_t *t) {
    if (!t || !t->px || t->dumped == g_frame_no) return;
    char p[700]; snprintf(p, sizeof p, "%s/tex%04d_%dx%d.png", g_dir, t->id, t->w, t->h);
    write_png_rgba(p, t->px, t->w, t->h); t->dumped = g_frame_no;
    /* how much of level 0 is empty (all zero) - a half-filled texture page shows here */
    long z = 0; for (long i = 0; i < (long)t->w * t->h; i++) z += t->px[i] == 0 || t->px[i] == 0xff000000u;
    fprintf(g_ft, "TEX #%d %dx%d levels=%d uploaded-levels=%x black=%.0f%%\n", t->id, t->w, t->h, t->levels, t->lvl_seen, 100.0 * z / ((double)t->w * t->h));
}
static void f_draw(rtarget_t *t, const rstate_t *s, int prim, const rvert_t *v, int nv, const uint16_t *idx, int ni) {
    if (g_recording) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e9f, z1 = -1e9f, w0 = 1e9f, w1 = -1e9f;
        ftgt_t *tg0 = (ftgt_t *)t; int wone = 0;
        for (int i = 0; i < nv; i++) {
            float w = v[i].w != 0 ? v[i].w : 1, x = (v[i].x / w + 1) * 0.5f * tg0->w, y = (1 - v[i].y / w) * 0.5f * tg0->h, z = v[i].z / w;
            if (v[i].w == 1.0f) wone++;
            if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
            if (z < z0) z0 = z; if (z > z1) z1 = z; if (w < w0) w0 = w; if (w > w1) w1 = w;
        }
        ftgt_t *tg = (ftgt_t *)t;
        float bw = (x1 < tg->w ? x1 : tg->w) - (x0 > 0 ? x0 : 0), bh = (y1 < tg->h ? y1 : tg->h) - (y0 > 0 ? y0 : 0);
        float cover = bw > 0 && bh > 0 ? bw * bh / ((float)tg->w * tg->h) : 0;
        ftex_t *t0 = (ftex_t *)s->st[0].tex, *t1 = s->nstages > 1 ? (ftex_t *)s->st[1].tex : NULL;
        fprintf(g_ft, "DRAW #%d target#%d prim=%d nv=%d ni=%d screen=[%.0f,%.0f]-[%.0f,%.0f] cover=%.1f%% z=%.4f..%.4f w=%.3f..%.3f w1=%d "
                "vp=%d,%d %dx%d tex0=%d(%dx%d a%d k%d op%d/%d,%d aop%d) tex1=%d blend=%d %d/%d atest=%d f%d ref%u zen=%d zw=%d zf=%d cull=%d fog=%d/%d c%08x spec=%d ckey=%d diff0=%08x spec0=%08x\n",
                g_ndraw++, tg->id, prim, nv, ni, x0, y0, x1, y1, cover * 100, z0, z1, w0, w1, wone, s->vx, s->vy, s->vw, s->vh,
                t0 ? t0->id : 0, t0 ? t0->w : 0, t0 ? t0->h : 0, s->st[0].has_alpha, s->st[0].has_ckey, s->st[0].colorop, s->st[0].carg1, s->st[0].carg2, s->st[0].alphaop,
                t1 ? t1->id : 0, s->blend, s->srcblend, s->dstblend, s->atest, s->afunc, s->aref, s->zenable, s->zwrite, s->zfunc, s->cull,
                s->fog, s->fogtable, s->fogcolor, s->specular, s->ckey, nv ? v[0].diffuse : 0, nv ? v[0].specular : 0);
        for (int k = 0; k < s->nstages && k < 2; k++) {
            const rstage_t *q = &s->st[k];
            float u0 = 1e9f, u1 = -1e9f, w0_ = 1e9f, w1_ = -1e9f; int tc = q->texcoord & 1;
            for (int i = 0; i < nv; i++) { float u = v[i].uv[tc][0], vv = v[i].uv[tc][1]; if (u < u0) u0 = u; if (u > u1) u1 = u; if (vv < w0_) w0_ = vv; if (vv > w1_) w1_ = vv; }
            fprintf(g_ft, "   st%d op%d(%x,%x) aop%d(%x,%x) tci=%d addr=%d/%d filt=%d/%d/%d bias=%.2f uv=[%.3f..%.3f]x[%.3f..%.3f]\n", k, q->colorop, q->carg1, q->carg2,
                    q->alphaop, q->aarg1, q->aarg2, q->texcoord, q->addru, q->addrv, q->magf, q->minf, q->mipf, q->lodbias, u0, u1, w0_, w1_);
        }
        dump_tex(t0); dump_tex(t1);
    }
    if (s->st[0].tex || s->st[1].tex) {
        rstate_t c = *s;
        for (int i = 0; i < 2; i++) if (c.st[i].tex) c.st[i].tex = ((ftex_t *)c.st[i].tex)->real;
        g_in->draw(T(t), &c, prim, v, nv, idx, ni);
    } else g_in->draw(T(t), s, prim, v, nv, idx, ni);
}
static void f_blit(rtarget_t *t, int dx, int dy, int dw, int dh, const uint32_t *p, int sw, int sh, int pitch, int keyed) {
    if (g_recording) fprintf(g_ft, "BLIT target#%d to %d,%d %dx%d from %dx%d keyed=%d\n", ((ftgt_t *)t)->id, dx, dy, dw, dh, sw, sh, keyed);
    g_in->blit(T(t), dx, dy, dw, dh, p, sw, sh, pitch, keyed);
}
static void f_copy(rtarget_t *t, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
    if (g_recording) fprintf(g_ft, "COPY target#%d %d,%d %dx%d <- %d,%d %dx%d\n", ((ftgt_t *)t)->id, dx, dy, dw, dh, sx, sy, sw, sh);
    g_in->copy(T(t), dx, dy, dw, dh, sx, sy, sw, sh);
}
static void f_copy_from(rtarget_t *t, int dx, int dy, int dw, int dh, rtarget_t *src, int sx, int sy, int sw, int sh) {
    if (g_recording) fprintf(g_ft, "COPYFROM target#%d %d,%d %dx%d <- target#%d %d,%d %dx%d\n", ((ftgt_t *)t)->id, dx, dy, dw, dh, ((ftgt_t *)src)->id, sx, sy, sw, sh);
    g_in->copy_from(T(t), dx, dy, dw, dh, T(src), sx, sy, sw, sh);
}
static void frame_boundary(const char *what, int w, int h) {
    if (g_recording) {
        fprintf(g_ft, "%s %dx%d\n", what, w, h);
        fclose(g_ft); g_ft = NULL; g_recording = 0;
        rt_log(0, "frame dump written to %s", g_dir);
    }
    if (g_armed) {
        g_armed = 0;
        snprintf(g_dir, sizeof g_dir, "%s/frame_%05d", g_base, ++g_count);
        while (1) { FILE *x = NULL; char t[700]; snprintf(t, sizeof t, "%s/frame.txt", g_dir); x = fopen(t, "r"); if (!x) break; fclose(x); snprintf(g_dir, sizeof g_dir, "%s/frame_%05d", g_base, ++g_count); }
        plat_mkdir(g_base); plat_mkdir(g_dir);
        char p[700]; snprintf(p, sizeof p, "%s/frame.txt", g_dir);
        g_ft = fopen(p, "w");
        if (g_ft) { g_recording = 1; g_ndraw = 0; g_frame_no++; fprintf(g_ft, "GP3RT frame dump (spray %d%%)\n", g_spray_opacity); }
    }
}
static void f_present(rtarget_t *t, int w, int h) { frame_boundary("PRESENT", w, h); g_in->present(T(t), w, h); }
static void f_present_argb(const uint32_t *p, int w, int h, int pitch) { frame_boundary("PRESENT_ARGB", w, h); g_in->present_argb(p, w, h, pitch); }

static rbackend_t g_fd = { "framedump", f_tex_create, f_tex_upload, f_tex_destroy, f_target_create, f_target_destroy,
                           f_target_upload, f_target_download, f_clear, f_draw, f_blit, f_copy, f_present, f_present_argb, f_copy_from };
rbackend_t *rdr_framedump_wrap(rbackend_t *inner, const char *dir) {
    g_in = inner; snprintf(g_base, sizeof g_base, "%s", dir);
    rt_log(1, "frame dump ready: Ctrl+F11 (or Ctrl+Print Screen) saves the next frame's draw list to %s", dir);
    return &g_fd;
}
