/* Capture wrapper: records every renderer call into a file (for replaying
 * through other backends in tests) and forwards it to the real backend. */
#include "rt.h"
#include "rdr.h"
#include "rdr_capture.h"
#include "rdr_serial.h"

static rbackend_t *g_in;
static FILE *g_cf;
double g_capture_from;   /* seconds: draw calls before this are not recorded */
static int live(void) { return rt_now_us() / 1e6 >= g_capture_from; }
enum { C_TEXC = 1, C_TEXU, C_TEXD, C_TGTC, C_TGTD, C_TGTU, C_TGTDL, C_CLEAR, C_DRAW, C_BLIT, C_PRESENT, C_PRESENTA };

/* objects are referenced by their address in this process */
static void w32(uint32_t v) { fwrite(&v, 4, 1, g_cf); }
static void wb(const void *p, size_t n) { fwrite(p, 1, n, g_cf); }

static rtex_t *c_tex_create(int w, int h, int l) { rtex_t *t = g_in->tex_create(w, h, l); w32(C_TEXC); w32((uint32_t)(uintptr_t)t); w32(w); w32(h); w32(l); return t; }
static void c_tex_upload(rtex_t *t, int l, int w, int h, const uint32_t *p) { w32(C_TEXU); w32((uint32_t)(uintptr_t)t); w32(l); w32(w); w32(h); wb(p, (size_t)w * h * 4); g_in->tex_upload(t, l, w, h, p); }
static void c_tex_destroy(rtex_t *t) { w32(C_TEXD); w32((uint32_t)(uintptr_t)t); g_in->tex_destroy(t); }
static rtarget_t *c_target_create(int w, int h, int s) { rtarget_t *t = g_in->target_create(w, h, s); w32(C_TGTC); w32((uint32_t)(uintptr_t)t); w32(w); w32(h); w32(s); return t; }
static void c_target_destroy(rtarget_t *t) { w32(C_TGTD); w32((uint32_t)(uintptr_t)t); g_in->target_destroy(t); }
static void c_target_upload(rtarget_t *t, const uint32_t *p, int pitch, int x, int y, int w, int h) {
    w32(C_TGTU); w32((uint32_t)(uintptr_t)t); w32(x); w32(y); w32(w); w32(h);
    for (int j = 0; j < h; j++) wb(p + (size_t)(y + j) * pitch + x, (size_t)w * 4);
    g_in->target_upload(t, p, pitch, x, y, w, h);
}
static void c_target_download(rtarget_t *t, uint32_t *p, int pitch, int y0, int n) { w32(C_TGTDL); w32((uint32_t)(uintptr_t)t); g_in->target_download(t, p, pitch, y0, n); }
static void c_clear(rtarget_t *t, const int *r, int n, int c, uint32_t argb, int d, float z, int vx, int vy, int vw, int vh) {
    if (!live()) goto fwd;
    w32(C_CLEAR); w32((uint32_t)(uintptr_t)t); w32(n); if (n) wb(r, (size_t)n * 16);
    w32(c); w32(argb); w32(d); wb(&z, 4); w32(vx); w32(vy); w32(vw); w32(vh);
fwd:
    g_in->clear(t, r, n, c, argb, d, z, vx, vy, vw, vh);
}
static void c_draw(rtarget_t *t, const rstate_t *s, int prim, const rvert_t *v, int nv, const uint16_t *idx, int ni) {
    if (!live()) goto fwd;
    w32(C_DRAW); w32((uint32_t)(uintptr_t)t);
    rstate_write(g_cf, s);
    w32(prim); w32(nv); wb(v, sizeof(rvert_t) * nv); w32(ni); wb(idx, 2u * ni);
fwd:
    g_in->draw(t, s, prim, v, nv, idx, ni);
}
static void c_blit(rtarget_t *t, int dx, int dy, int dw, int dh, const uint32_t *p, int sw, int sh, int pitch, int keyed) {
    if (!live()) goto fwd;
    w32(C_BLIT); w32((uint32_t)(uintptr_t)t); w32(dx); w32(dy); w32(dw); w32(dh); w32(sw); w32(sh); w32(keyed);
    for (int j = 0; j < sh; j++) wb(p + (size_t)j * pitch, (size_t)sw * 4);
fwd:
    g_in->blit(t, dx, dy, dw, dh, p, sw, sh, pitch, keyed);
}
static void c_copy(rtarget_t *t, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
    if (!live()) goto fwd;
    w32(13); w32((uint32_t)(uintptr_t)t); w32(dx); w32(dy); w32(dw); w32(dh); w32(sx); w32(sy); w32(sw); w32(sh);
fwd:
    g_in->copy(t, dx, dy, dw, dh, sx, sy, sw, sh);
}
static void c_copy_from(rtarget_t *t, int dx, int dy, int dw, int dh, rtarget_t *src, int sx, int sy, int sw, int sh) {
    if (!live()) goto fwd;
    w32(14); w32((uint32_t)(uintptr_t)t); w32(dx); w32(dy); w32(dw); w32(dh); w32((uint32_t)(uintptr_t)src); w32(sx); w32(sy); w32(sw); w32(sh);
fwd:
    g_in->copy_from(t, dx, dy, dw, dh, src, sx, sy, sw, sh);
}
static void c_present(rtarget_t *t, int w, int h) {
    if (!live()) goto fwd; w32(C_PRESENT); w32((uint32_t)(uintptr_t)t); w32(w); w32(h); fflush(g_cf);
fwd:
    g_in->present(t, w, h); }
static void c_present_argb(const uint32_t *p, int w, int h, int pitch) {
    if (!live()) goto fwd;
    w32(C_PRESENTA); w32(w); w32(h);
    for (int j = 0; j < h; j++) wb(p + (size_t)j * pitch, (size_t)w * 4);
    fflush(g_cf);
fwd:
    g_in->present_argb(p, w, h, pitch);
}

static rbackend_t g_cap = { "capture", c_tex_create, c_tex_upload, c_tex_destroy, c_target_create, c_target_destroy,
                            c_target_upload, c_target_download, c_clear, c_draw, c_blit, c_copy, c_present, c_present_argb, c_copy_from };

rbackend_t *rdr_capture_wrap(rbackend_t *inner, const char *path) {
    g_cf = fopen(path, "wb");
    if (!g_cf) return inner;
    g_in = inner;
    fwrite("GP3RCAP2", 1, 8, g_cf);
    w32(sizeof(rvert_t));
    return &g_cap;
}
