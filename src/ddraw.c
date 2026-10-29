/* DirectDraw 7 implementation.
 * Every surface lives in system memory in its native format (8-bit palettised,
 * 16-bit 565/555, 24/32-bit); the primary surface is converted to 32-bit
 * colour at presentation time. This removes any dependence on the display
 * modes and DirectDraw emulation of modern Windows versions. */
#include "ddraw_int.h"
#include "gamepatch.h"
#include "apinote.h"
#include "plat.h"
#include "gdi.h"

os_mutex_t g_dd_lock;
uint32_t g_scratch_site;
void *g_dd_caller;   /* game return address of the current DirectDraw call (diagnostics) */
uint32_t g_diff_site; static uint8_t *g_diff_snap; static ddsurf_t *g_diff_surf; static uint32_t *g_diff_acc; static int g_diff_w, g_diff_h, g_diff_n;

ddraw_t *g_ddraw;
static void **g_dd7_vtbl, **g_dds7_vtbl, **g_pal_vtbl, **g_clip_vtbl, **g_gamma_vtbl;

int guid_eq(const uint8_t *g, const char *s) {
    unsigned a, b, c, d[8];
    if (!g) return 0;
    sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
    uint8_t x[16];
    memcpy(x, &a, 4); uint16_t bb = b, cc = c; memcpy(x + 4, &bb, 2); memcpy(x + 6, &cc, 2);
    for (int i = 0; i < 8; i++) x[8 + i] = (uint8_t)d[i];
    return !memcmp(g, x, 16);
}
void guid_str(const uint8_t *g, char *o) {
    if (!g) { strcpy(o, "NULL"); return; }
    sprintf(o, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", *(uint32_t *)g, *(uint16_t *)(g + 4), *(uint16_t *)(g + 6),
            g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}
#define IID_IUnknown        "00000000-0000-0000-C000-000000000046"
#define IID_IDirectDraw     "6C14DB80-A733-11CE-A521-0020AF0BE560"
#define IID_IDirectDraw2    "B3A6F3E0-2B43-11CF-A2DE-00AA00B93356"
#define IID_IDirectDraw4    "9C59509A-39BD-11D1-8C4A-00C04FD930C5"
#define IID_IDirectDraw7    "15E65EC0-3B9C-11D2-B92F-00609797EA5B"
#define IID_IDirect3D7      "F5049E77-4861-11D2-A407-00A0C90629A8"
#define IID_IDDSurface      "6C14DB81-A733-11CE-A521-0020AF0BE560"
#define IID_IDDSurface2     "57805885-6EEC-11CF-9441-A82303C10E27"
#define IID_IDDSurface3     "DA044E00-69B2-11D0-A1D5-00AA00B8DFBB"
#define IID_IDDSurface4     "0B2B8630-AD35-11D0-8EA6-00609797EA5B"
#define IID_IDDSurface7     "06675A80-3B9B-11D2-B92F-00609797EA5B"
#define IID_IDDGammaControl "69C11C3E-B46B-11D1-AD7A-00C04FC29B4E"

/* ------------------------------------------------------------------ */
/* pixel formats                                                        */
void pf_from_bpp(DDPIXELFORMAT *pf, int bpp) {
    memset(pf, 0, sizeof *pf);
    pf->size = 32;
    pf->bpp = bpp;
    switch (bpp) {
    case 8: pf->flags = DDPF_RGB | DDPF_PALETTEINDEXED8; break;
    case 16: pf->flags = DDPF_RGB; pf->rmask = 0xf800; pf->gmask = 0x07e0; pf->bmask = 0x001f; break;
    case 24: pf->flags = DDPF_RGB; pf->rmask = 0xff0000; pf->gmask = 0xff00; pf->bmask = 0xff; break;
    case 32: pf->flags = DDPF_RGB; pf->rmask = 0xff0000; pf->gmask = 0xff00; pf->bmask = 0xff; break;
    }
}

/* 8 bits per channel from a masked value */
static inline uint32_t mask_expand(uint32_t v, uint32_t m) {
    if (!m) return 0;
    int sh = __builtin_ctz(m); int bits = __builtin_popcount(m);
    uint32_t x = (v & m) >> sh;
    if (bits >= 8) return x >> (bits - 8);
    return (x << (8 - bits)) | (x >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0));
}
static inline uint32_t mask_pack(uint32_t c8, uint32_t m) {
    if (!m) return 0;
    int sh = __builtin_ctz(m); int bits = __builtin_popcount(m);
    uint32_t x = bits >= 8 ? c8 << (bits - 8) : c8 >> (8 - bits);
    return (x << sh) & m;
}
static inline uint32_t raw_get(ddsurf_t *s, int x, int y) {
    uint8_t *row = s->bits + (ptrdiff_t)y * s->pitch;
    switch (s->bpp) {
    case 8: return row[x];
    case 16: return ((uint16_t *)row)[x];
    case 24: return row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16;
    default: return ((uint32_t *)row)[x];
    }
}
static inline void raw_put(ddsurf_t *s, int x, int y, uint32_t v) {
    uint8_t *row = s->bits + (ptrdiff_t)y * s->pitch;
    switch (s->bpp) {
    case 8: row[x] = (uint8_t)v; break;
    case 16: ((uint16_t *)row)[x] = (uint16_t)v; break;
    case 24: row[x * 3] = v; row[x * 3 + 1] = v >> 8; row[x * 3 + 2] = v >> 16; break;
    default: ((uint32_t *)row)[x] = v; break;
    }
}
static inline uint32_t raw_to_xrgb(ddsurf_t *s, uint32_t v) {
    if (s->bpp == 8) return s->pal ? s->pal->xrgb[v & 255] : (v * 0x010101u);
    return mask_expand(v, s->pf.rmask) << 16 | mask_expand(v, s->pf.gmask) << 8 | mask_expand(v, s->pf.bmask);
}
static inline uint32_t xrgb_to_raw(ddsurf_t *s, uint32_t c, uint32_t a8) {
    if (s->bpp == 8) {
        if (!s->pal) return 0;
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < 256; i++) {
            uint32_t p = s->pal->xrgb[i];
            int dr = (int)((c >> 16) & 255) - (int)((p >> 16) & 255), dg = (int)((c >> 8) & 255) - (int)((p >> 8) & 255), db = (int)(c & 255) - (int)(p & 255);
            int d = dr * dr + dg * dg + db * db; if (d < bd) { bd = d; best = i; }
        }
        return best;
    }
    return mask_pack((c >> 16) & 255, s->pf.rmask) | mask_pack((c >> 8) & 255, s->pf.gmask) | mask_pack(c & 255, s->pf.bmask) | mask_pack(a8, s->pf.amask);
}
uint32_t dds_pixel_xrgb(ddsurf_t *s, int x, int y) { return raw_to_xrgb(s, raw_get(s, x, y)); }

/* ------------------------------------------------------------------ */
/* presentation                                                         */
static uint64_t g_last_present_us;
static uint16_t g_gamma[3][256];
static int g_gamma_set;
static uint32_t *g_present_buf;
static size_t g_present_cap;

static ddsurf_t *g_pending_present;
static void dds_present_now(ddsurf_t *s);
/* Surface memory gets generous slack after the image: GP3 writes past the end of
 * some surfaces (e.g. a 256x256 bitmap into a 256x128 texture); DirectDraw drivers
 * of the time tolerated it, an exact-size allocation does not. */
static size_t surf_alloc_size(int pitch, int h) { size_t n = (size_t)pitch * h; return n * 2 + 65536; }
void ddraw_run_deferred(void);
/* Presentation requests from CPU-side changes of the visible surface (Unlock,
 * Blt, palette changes) are coalesced: GP3's menus touch the front buffer
 * hundreds of times per second. Flips always present immediately. */
void dds_present(ddsurf_t *s) {
    if (!s) return;
    uint64_t now = rt_now_us();
    if (now - g_last_present_us >= 8000) { g_pending_present = NULL; dds_present_now(s); }
    else g_pending_present = s;
}
/* present requested by a thread that may not touch the GPU: done later by the render thread */
static ddsurf_t *volatile g_foreign_present;
extern int rdr_foreign_thread(void);
extern int rdr_held_elsewhere(void);
void ddraw_flush_present(void);
void ddraw_run_deferred(void) {
    if (g_pending_present) ddraw_flush_present();
    if (!g_foreign_present || rdr_held_elsewhere() || rdr_foreign_thread()) return;
    DDLOCK();
    ddsurf_t *s = g_foreign_present; g_foreign_present = NULL;
    if (s && g_ddraw && g_ddraw->primary == s) dds_present_now(s);
    DDUNLOCK();
}
void ddraw_flush_present(void) {
    if (g_foreign_present) ddraw_run_deferred();
    if (!g_pending_present) return;
    if (rdr_held_elsewhere()) return;     /* the thread holding the renderer will show it */
    DDLOCK();
    ddsurf_t *s = g_pending_present;
    if (s && g_ddraw && g_ddraw->primary == s && rt_now_us() - g_last_present_us >= 8000) { g_pending_present = NULL; dds_present_now(s); }
    DDUNLOCK();
}
/* from message pumps (PeekMessage/GetMessage hooks): never wait for the DirectDraw lock.
 * With a real GPU the presenting thread holds it while SwapBuffers waits for vsync;
 * blocking here would starve the window's message loop (keyboard/mouse, "not responding"). */
void ddraw_flush_present_nb(void) {
    if (!g_pending_present && !g_foreign_present) return;
    if (rdr_held_elsewhere()) return;
    if (!os_mutex_trylock(&g_dd_lock)) return;
    ddraw_flush_present();
    DDUNLOCK();
}
static void dds_present_now(ddsurf_t *s) {
    extern int d3d_present_hook(ddsurf_t *front);
    if (rdr_foreign_thread()) {
        static int told;
        if (!told) { told = 1; LOG("present from a secondary thread: deferred to the render thread"); }
        g_foreign_present = s; g_last_present_us = rt_now_us(); return;
    }
    { static unsigned seen[4]; unsigned t = os_thread_id(); int k;
      for (k = 0; k < 4 && seen[k] && seen[k] != t; k++) {}
      if (k < 4 && !seen[k]) { seen[k] = t; LOG("present from thread %u", t); }
    }
    { extern uint32_t g_present_calls; g_present_calls++; }
    if (d3d_present_hook(s)) return;
    size_t need = (size_t)s->w * s->h;
    if (need > g_present_cap) { free(g_present_buf); g_present_buf = malloc(need * 4); g_present_cap = need; }
    uint32_t *o = g_present_buf;
    if (s->bpp == 8) {
        uint32_t lut[256];
        for (int i = 0; i < 256; i++) lut[i] = s->pal ? s->pal->xrgb[i] : i * 0x010101u;
        for (int y = 0; y < s->h; y++) { uint8_t *r = s->bits + (ptrdiff_t)y * s->pitch; for (int x = 0; x < s->w; x++) *o++ = lut[r[x]]; }
    } else if (s->bpp == 16 && s->pf.gmask == 0x7e0) {
        for (int y = 0; y < s->h; y++) {
            uint16_t *r = (uint16_t *)(s->bits + (ptrdiff_t)y * s->pitch);
            for (int x = 0; x < s->w; x++) {
                uint32_t p = r[x];
                uint32_t R = (p >> 11) & 31, G = (p >> 5) & 63, B = p & 31;
                *o++ = ((R << 3 | R >> 2) << 16) | ((G << 2 | G >> 4) << 8) | (B << 3 | B >> 2);
            }
        }
    } else {
        for (int y = 0; y < s->h; y++) for (int x = 0; x < s->w; x++) *o++ = raw_to_xrgb(s, raw_get(s, x, y));
    }
    if (g_diff_acc && g_diff_w == s->w && g_diff_h == s->h) {
        for (size_t i = 0; i < need; i++) if (g_diff_acc[i]) g_present_buf[i] = 0xff0000;
        memset(g_diff_acc, 0, need * 4);
    }
    if (g_gamma_set) {
        for (size_t i = 0; i < need; i++) {
            uint32_t c = g_present_buf[i];
            g_present_buf[i] = (uint32_t)(g_gamma[0][(c >> 16) & 255] >> 8) << 16 | (uint32_t)(g_gamma[1][(c >> 8) & 255] >> 8) << 8 | (g_gamma[2][c & 255] >> 8);
        }
    }
    extern void rdr_present_cpu(const uint32_t *argb, int w, int h, int pitch);
    rdr_present_cpu(g_present_buf, s->w, s->h, s->w);
    g_last_present_us = rt_now_us();
}

/* emulate the vertical blank of a 60 Hz display (games used Flip(WAIT) for pacing).
 * Disabled when the presentation itself is synchronised to the display (OpenGL + vsync). */
int g_emulate_vblank = 1;
static void wait_vblank(void) {
    if (!g_emulate_vblank) return;
    const uint64_t period = 16667;
    uint64_t now = rt_now_us();
    uint64_t next = (now / period + 1) * period;
    os_sleep_us(next - now);
}

/* ------------------------------------------------------------------ */
/* COM helpers                                                          */
#define THIS_DD ddraw_t *This
#define THIS_S ddsurf_t *This

static void **vtbl_alloc(int n) { return calloc(n, sizeof(void *)); }

/* ------------------------------------------------------------------ */
/* palette                                                              */
static HRESULT WINAPI pal_QueryInterface(ddpal_t *This, const uint8_t *iid, void **out) { API_NOTE("pal_QueryInterface"); (void)iid; *out = This; This->refs++; return 0; }
static DWORD WINAPI pal_AddRef(ddpal_t *This) { API_NOTE("pal_AddRef"); return ++This->refs; }
static DWORD WINAPI pal_Release(ddpal_t *This) { API_NOTE("pal_Release"); DWORD r = --This->refs; if (!r) free(This); return r; }
static HRESULT WINAPI pal_GetCaps(ddpal_t *This, DWORD *c) { API_NOTE("pal_GetCaps"); *c = This->caps; return 0; }
static HRESULT WINAPI pal_GetEntries(ddpal_t *This, DWORD fl, DWORD start, DWORD n, uint8_t *out) { API_NOTE("pal_GetEntries");
    (void)fl;
    for (DWORD i = 0; i < n && start + i < 256; i++) memcpy(out + i * 4, This->raw[start + i], 4);
    return 0;
}
static HRESULT WINAPI pal_Initialize(ddpal_t *This, void *dd, DWORD fl, void *e) { API_NOTE("pal_Initialize"); (void)This; (void)dd; (void)fl; (void)e; return 0x887600F5; }
static void pal_set(ddpal_t *p, DWORD start, DWORD n, const uint8_t *e) {
    for (DWORD i = 0; i < n && start + i < 256; i++) {
        memcpy(p->raw[start + i], e + i * 4, 4);
        p->xrgb[start + i] = (uint32_t)e[i * 4] << 16 | (uint32_t)e[i * 4 + 1] << 8 | e[i * 4 + 2];
    }
    p->version++;
}
static HRESULT WINAPI pal_SetEntries(ddpal_t *This, DWORD fl, DWORD start, DWORD n, const uint8_t *e) { API_NOTE("pal_SetEntries");
    (void)fl;
    DDLOCK();
    pal_set(This, start, n, e);
    /* palette animation on the visible surface: show it */
    ddsurf_t *pr = g_ddraw ? g_ddraw->primary : NULL;
    if (pr && pr->pal == This && pr->bpp == 8) dds_present(pr);
    DDUNLOCK();
    return 0;
}

/* clipper */
static HRESULT WINAPI clip_QueryInterface(ddclip_t *This, const uint8_t *iid, void **out) { API_NOTE("clip_QueryInterface"); (void)iid; *out = This; This->refs++; return 0; }
static DWORD WINAPI clip_AddRef(ddclip_t *This) { API_NOTE("clip_AddRef"); return ++This->refs; }
static DWORD WINAPI clip_Release(ddclip_t *This) { API_NOTE("clip_Release"); DWORD r = --This->refs; if (!r) free(This); return r; }
static HRESULT WINAPI clip_GetClipList(ddclip_t *This, RECT *r, void *list, DWORD *sz) { API_NOTE("clip_GetClipList"); (void)This; (void)r; (void)list; if (sz) *sz = 0; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI clip_GetHWnd(ddclip_t *This, HWND *h) { API_NOTE("clip_GetHWnd"); *h = This->hwnd; return 0; }
static HRESULT WINAPI clip_Initialize(ddclip_t *This, void *dd, DWORD f) { API_NOTE("clip_Initialize"); (void)This; (void)dd; (void)f; return 0; }
static HRESULT WINAPI clip_IsClipListChanged(ddclip_t *This, BOOL *b) { API_NOTE("clip_IsClipListChanged"); (void)This; *b = FALSE; return 0; }
static HRESULT WINAPI clip_SetClipList(ddclip_t *This, void *l, DWORD f) { API_NOTE("clip_SetClipList"); (void)This; (void)l; (void)f; return 0; }
static HRESULT WINAPI clip_SetHWnd(ddclip_t *This, DWORD f, HWND h) { API_NOTE("clip_SetHWnd"); (void)f; This->hwnd = h; return 0; }

/* gamma control (attached to the primary surface) */
typedef struct { void **vtbl; LONG refs; ddsurf_t *s; } ddgamma_t;
static HRESULT WINAPI gam_QueryInterface(ddgamma_t *This, const uint8_t *iid, void **out) { (void)iid; *out = This; This->refs++; return 0; }
static DWORD WINAPI gam_AddRef(ddgamma_t *This) { return ++This->refs; }
static DWORD WINAPI gam_Release(ddgamma_t *This) { return --This->refs; }
static HRESULT WINAPI gam_GetGammaRamp(ddgamma_t *This, DWORD f, uint16_t *r) {
    (void)This; (void)f;
    if (!g_gamma_set) for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) g_gamma[c][i] = (uint16_t)(i * 257);
    memcpy(r, g_gamma, sizeof g_gamma);
    return 0;
}
static HRESULT WINAPI gam_SetGammaRamp(ddgamma_t *This, DWORD f, const uint16_t *r) {
    (void)This; (void)f;
    memcpy(g_gamma, r, sizeof g_gamma);
    int ident = 1;
    for (int c = 0; c < 3 && ident; c++) for (int i = 0; i < 256; i++) if ((g_gamma[c][i] >> 8) != i) { ident = 0; break; }
    g_gamma_set = !ident;
    TRACE("SetGammaRamp: %s", ident ? "identity" : "custom");
    return 0;
}

/* ------------------------------------------------------------------ */
/* surfaces                                                             */
void dds_fill_desc(ddsurf_t *s, DDSURFACEDESC2 *d) {
    DWORD sz = d->size == 108 ? 108 : 124;
    memset(d, 0, sz);
    d->size = sz;
    d->flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->width = s->w; d->height = s->h; d->pitch = s->pitch;
    d->pf = s->pf;
    d->caps = s->caps;
    if (sz == 108) { d->caps.caps2 = 0; }
    if (s->has_ck_src) { d->flags |= DDSD_CKSRCBLT; d->ck_src_blt = s->ck_src; }
    if (s->has_ck_dst) { d->flags |= DDSD_CKDESTBLT; d->ck_dest_blt = s->ck_dst; }
    if (s->back) { d->flags |= DDSD_BACKBUFFERCOUNT; int n = 0; for (ddsurf_t *b = s->back; b && b != s && n < 8; b = b->back) n++; d->backbuffers = n; }
    if (s->caps.caps & DDSCAPS_MIPMAP) { d->flags |= DDSD_MIPMAPCOUNT; int n = 1; for (ddsurf_t *m = s->mip_next; m; m = m->mip_next) n++; d->mipmaps = n; }
}

static ddsurf_t *surf_alloc(ddraw_t *dd, int w, int h, const DDPIXELFORMAT *pf, DDSCAPS2 caps) {
    ddsurf_t *s = calloc(1, sizeof *s);
    s->vtbl = g_dds7_vtbl; s->refs = 1; s->dd = dd;
    s->w = w; s->h = h; s->pf = *pf; s->bpp = pf->bpp ? (int)pf->bpp : 16;
    if (s->pf.flags & DDPF_ZBUFFER) s->bpp = pf->bpp;
    s->caps = caps;
    s->pitch = ((w * ((s->bpp + 7) / 8)) + 3) & ~3;
    /* surfaces that can be 3D render targets get page-sized rows (lazy readback, see d3d.c) */
    if (caps.caps & (DDSCAPS_3DDEVICE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) s->pitch = (s->pitch + 4095) & ~4095;
    s->bits = surf_mem_alloc(surf_alloc_size(s->pitch, h));
    s->own_bits = 1;
    s->lod = 0;
    return s;
}

static int is_pow2(int v) { return v > 0 && !(v & (v - 1)); }

ddsurf_t *dds_create(ddraw_t *dd, const DDSURFACEDESC2 *d) {
    DDSCAPS2 caps = (d->flags & DDSD_CAPS) ? d->caps : (DDSCAPS2){ DDSCAPS_OFFSCREENPLAIN, 0, 0, 0 };
    DDPIXELFORMAT pf;
    int w = (d->flags & DDSD_WIDTH) ? (int)d->width : 0;
    int h = (d->flags & DDSD_HEIGHT) ? (int)d->height : 0;
    if (caps.caps & DDSCAPS_PRIMARYSURFACE) { w = dd->mode_w; h = dd->mode_h; }
    if (d->flags & DDSD_PIXELFORMAT) pf = d->pf;
    else if (caps.caps & DDSCAPS_ZBUFFER) { memset(&pf, 0, sizeof pf); pf.size = 32; pf.flags = DDPF_ZBUFFER; pf.bpp = (d->flags & DDSD_ZBUFFERBITDEPTH) ? d->backbuffers : 16; pf.rmask = pf.bpp == 16 ? 0xffff : 0xffffff; }
    else pf_from_bpp(&pf, dd->mode_bpp);
    if (pf.flags & DDPF_ZBUFFER) { if (!pf.bpp) pf.bpp = 16; }
    else if (pf.flags & DDPF_RGB && !pf.rmask && pf.bpp != 8) { int b = pf.bpp; DWORD fl = pf.flags; pf_from_bpp(&pf, b); pf.flags |= fl & DDPF_ALPHAPIXELS; }
    if (w <= 0 || h <= 0) { WARN("CreateSurface: bad size %dx%d", w, h); return NULL; }
    /* everything is "video memory" from the game's point of view */
    if (!(caps.caps & DDSCAPS_SYSTEMMEMORY)) caps.caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    ddsurf_t *s = surf_alloc(dd, w, h, &pf, caps);
    s->sd_flags = d->flags;
    if (d->flags & DDSD_CKSRCBLT) { s->ck_src = d->ck_src_blt; s->has_ck_src = 1; }
    if (d->flags & DDSD_CKDESTBLT) { s->ck_dst = d->ck_dest_blt; s->has_ck_dst = 1; }
    if (caps.caps & DDSCAPS_PRIMARYSURFACE) {
        s->is_primary = 1;
        s->caps.caps |= DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER;
        dd->primary = s;
        int nb = (d->flags & DDSD_BACKBUFFERCOUNT) ? (int)d->backbuffers : 0;
        ddsurf_t *prev = s;
        for (int i = 0; i < nb; i++) {
            DDSCAPS2 bc = caps; bc.caps &= ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER);
            if (i == 0) bc.caps |= DDSCAPS_BACKBUFFER;
            ddsurf_t *b = surf_alloc(dd, w, h, &pf, bc);
            b->chain_owner = s;
            prev->back = b; prev = b;
        }
        if (nb) prev->back = s;   /* circular chain */
    }
    if ((caps.caps & DDSCAPS_MIPMAP) && (d->flags & DDSD_MIPMAPCOUNT) && d->mipmaps > 1) {
        ddsurf_t *prev = s; int mw = w, mh = h;
        for (DWORD i = 1; i < d->mipmaps; i++) {
            mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
            DDSCAPS2 mc = caps; mc.caps2 |= 0x10000; /* DDSCAPS2_MIPMAPSUBLEVEL */
            ddsurf_t *m = surf_alloc(dd, mw, mh, &pf, mc);
            m->chain_owner = s;
            prev->mip_next = m; prev = m;
        }
    }
    (void)is_pow2;
    return s;
}

static HRESULT WINAPI dds_QueryInterface(THIS_S, const uint8_t *iid, void **out) { API_NOTE("dds_QueryInterface");
    char g[64]; guid_str(iid, g);
    if (guid_eq(iid, IID_IDDSurface7) || guid_eq(iid, IID_IDDSurface4) || guid_eq(iid, IID_IDDSurface3) ||
        guid_eq(iid, IID_IDDSurface2) || guid_eq(iid, IID_IDDSurface) || guid_eq(iid, IID_IUnknown)) {
        *out = This; This->refs++; return 0;
    }
    if (guid_eq(iid, IID_IDDGammaControl)) {
        if (!This->gamma) { ddgamma_t *gm = calloc(1, sizeof *gm); gm->vtbl = g_gamma_vtbl; gm->refs = 1; gm->s = This; This->gamma = gm; }
        ((ddgamma_t *)This->gamma)->refs++;
        *out = This->gamma; return 0;
    }
    LOG("Surface::QueryInterface(%s) -> E_NOINTERFACE", g);
    *out = NULL;
    return E_NOINTERFACE;
}
static DWORD WINAPI dds_AddRef(THIS_S) { API_NOTE("dds_AddRef"); return ++This->refs; }
extern void d3d_surface_released(ddsurf_t *s);
static void surf_free(ddsurf_t *s) {
    d3d_surface_released(s);
    if (s->own_bits) surf_mem_free(s->bits, surf_alloc_size(s->pitch, s->h));
    if (s->pal) s->pal->refs--;
    if (s->dd && s->dd->primary == s) s->dd->primary = NULL;
    free(s);
}
static DWORD WINAPI dds_Release(THIS_S) { API_NOTE("dds_Release");
    DWORD r = --This->refs;
    if (!r) {
        DDLOCK();
        /* release attached chain owned by this surface */
        if (This->back) { ddsurf_t *b = This->back; while (b && b != This) { ddsurf_t *n = b->back; if (b->chain_owner == This) surf_free(b); b = n; } }
        ddsurf_t *m = This->mip_next; while (m) { ddsurf_t *n = m->mip_next; if (m->chain_owner == This) surf_free(m); m = n; }
        if (This->zbuf) dds_Release(This->zbuf);
        surf_free(This);
        DDUNLOCK();
    }
    return r;
}
static HRESULT WINAPI dds_AddAttachedSurface(THIS_S, ddsurf_t *a) { API_NOTE("dds_AddAttachedSurface");
    if (!a) return DDERR_INVALIDPARAMS;
    if (a->caps.caps & DDSCAPS_ZBUFFER) { if (This->zbuf) dds_Release(This->zbuf); This->zbuf = a; a->refs++; return 0; }
    LOG("AddAttachedSurface: non-zbuffer attach");
    This->back = a; a->refs++;
    return 0;
}
static HRESULT WINAPI dds_AddOverlayDirtyRect(THIS_S, RECT *r) { API_NOTE("dds_AddOverlayDirtyRect"); (void)This; (void)r; return DDERR_UNSUPPORTED; }

/* ---- blitting ---- */
typedef struct { DWORD size, ddfx, rop, ddrop, rot, zop, zlow, zhigh, zbase, zdcbd; uint32_t zdc; DWORD zscbd, zsc, aebd, aeb, res, adcbd, adc, ascbd, asc;
                 DWORD fill; DDCOLORKEY ck_dst, ck_src; } DDBLTFX;
#define DDBLT_COLORFILL 0x400
#define DDBLT_DDFX 0x800
#define DDBLT_KEYDEST 0x2000
#define DDBLT_KEYSRC 0x8000
#define DDBLT_KEYSRCOVERRIDE 0x10000
#define DDBLT_KEYDESTOVERRIDE 0x4000
#define DDBLT_DEPTHFILL 0x2000000
#define DDBLTFX_MIRRORLEFTRIGHT 2
#define DDBLTFX_MIRRORUPDOWN 4

static void fill_rect(ddsurf_t *d, RECT r, uint32_t v) {
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > d->w) r.right = d->w;
    if (r.bottom > d->h) r.bottom = d->h;
    for (int y = r.top; y < r.bottom; y++) {
        if (d->bpp == 8) memset(d->bits + (ptrdiff_t)y * d->pitch + r.left, (int)v, r.right - r.left);
        else if (d->bpp == 16) { uint16_t *p = (uint16_t *)(d->bits + (ptrdiff_t)y * d->pitch); for (int x = r.left; x < r.right; x++) p[x] = (uint16_t)v; }
        else for (int x = r.left; x < r.right; x++) raw_put(d, x, y, v);
    }
    d->version++;
}

extern void d3d_cpu_need(ddsurf_t *s);
extern void d3d_cpu_written(ddsurf_t *s);
static HRESULT blit2(ddsurf_t *d, const RECT *dr_, ddsurf_t *s, const RECT *sr_, DWORD flags, const DDBLTFX *fx);
extern int d3d_gpu_blit(ddsurf_t *d, const RECT *dr, ddsurf_t *s, const RECT *sr, DWORD flags, const void *fx);
extern void d3d_cpu_need_rows(ddsurf_t *s, int y0, int y1);
extern void d3d_cpu_written_rows(ddsurf_t *s, int y0, int y1);
static HRESULT blit(ddsurf_t *d, const RECT *dr_, ddsurf_t *s, const RECT *sr_, DWORD flags, const DDBLTFX *fx) {
    if (d3d_gpu_blit(d, dr_, s, sr_, flags, fx)) { d->version++; return 0; }   /* done by the renderer */
    /* CPU blit: only the rows involved need to be current in memory */
    RECT dr = dr_ ? *dr_ : (RECT){ 0, 0, d->w, d->h };
    RECT sr = s ? (sr_ ? *sr_ : (RECT){ 0, 0, s->w, s->h }) : (RECT){ 0, 0, 0, 0 };
    if (!(flags & DDBLT_DEPTHFILL)) {
        d3d_cpu_need_rows(d, dr.top, dr.bottom);
        if (s) d3d_cpu_need_rows(s, sr.top, sr.bottom);
    }
    HRESULT r = blit2(d, dr_, s, sr_, flags, fx);
    if (!(flags & DDBLT_DEPTHFILL)) d3d_cpu_written_rows(d, dr.top, dr.bottom);
    return r;
}
static HRESULT blit2(ddsurf_t *d, const RECT *dr_, ddsurf_t *s, const RECT *sr_, DWORD flags, const DDBLTFX *fx) {
    RECT dr = dr_ ? *dr_ : (RECT){ 0, 0, d->w, d->h };
    if (flags & DDBLT_COLORFILL) {
        fill_rect(d, dr, fx ? fx->fill : 0);
        return 0;
    }
    if (flags & DDBLT_DEPTHFILL) {
        uint32_t v = fx ? fx->fill : 0xffff;
        extern void d3d_depth_fill(ddsurf_t *z, RECT r, uint32_t v);
        d3d_depth_fill(d, dr, v);
        fill_rect(d, dr, v);
        return 0;
    }
    if (!s) return DDERR_INVALIDPARAMS;
    RECT sr = sr_ ? *sr_ : (RECT){ 0, 0, s->w, s->h };
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top, sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return 0;
    int keysrc = 0; uint32_t klo = 0, khi = 0;
    if (flags & DDBLT_KEYSRCOVERRIDE) { keysrc = 1; klo = fx->ck_src.lo; khi = fx->ck_src.hi; }
    else if ((flags & DDBLT_KEYSRC) && s->has_ck_src) { keysrc = 1; klo = s->ck_src.lo; khi = s->ck_src.hi; }
    if (khi < klo) khi = klo;
    int keydst = 0; uint32_t dlo = 0, dhi = 0;
    if (flags & DDBLT_KEYDESTOVERRIDE) { keydst = 1; dlo = fx->ck_dst.lo; dhi = fx->ck_dst.hi; }
    else if ((flags & DDBLT_KEYDEST) && d->has_ck_dst) { keydst = 1; dlo = d->ck_dst.lo; dhi = d->ck_dst.hi; }
    if (dhi < dlo) dhi = dlo;
    int mirx = (flags & DDBLT_DDFX) && fx && (fx->ddfx & DDBLTFX_MIRRORLEFTRIGHT);
    int miry = (flags & DDBLT_DDFX) && fx && (fx->ddfx & DDBLTFX_MIRRORUPDOWN);
    int same = d->bpp == s->bpp && (d->bpp == 8 || (d->pf.rmask == s->pf.rmask && d->pf.gmask == s->pf.gmask && d->pf.bmask == s->pf.bmask));
    int Bpp = (d->bpp + 7) / 8;
    /* fast path: same format, no stretch, no key, no mirror */
    if (same && dw == sw && dh == sh && !keysrc && !keydst && !mirx && !miry) {
        int x0 = dr.left, y0 = dr.top, sx0 = sr.left, sy0 = sr.top, w = dw, h = dh;
        if (x0 < 0) { sx0 -= x0; w += x0; x0 = 0; }
        if (y0 < 0) { sy0 -= y0; h += y0; y0 = 0; }
        if (sx0 < 0) { x0 -= sx0; w += sx0; sx0 = 0; }
        if (sy0 < 0) { y0 -= sy0; h += sy0; sy0 = 0; }
        if (x0 + w > d->w) w = d->w - x0;
        if (y0 + h > d->h) h = d->h - y0;
        if (sx0 + w > s->w) w = s->w - sx0;
        if (sy0 + h > s->h) h = s->h - sy0;
        if (w <= 0 || h <= 0) return 0;
        if (d == s && y0 > sy0) {
            for (int y = h - 1; y >= 0; y--) memmove(d->bits + (ptrdiff_t)(y0 + y) * d->pitch + x0 * Bpp, s->bits + (ptrdiff_t)(sy0 + y) * s->pitch + sx0 * Bpp, (size_t)w * Bpp);
        } else {
            for (int y = 0; y < h; y++) memmove(d->bits + (ptrdiff_t)(y0 + y) * d->pitch + x0 * Bpp, s->bits + (ptrdiff_t)(sy0 + y) * s->pitch + sx0 * Bpp, (size_t)w * Bpp);
        }
        d->version++;
        return 0;
    }
    for (int y = 0; y < dh; y++) {
        int Y = dr.top + y; if (Y < 0 || Y >= d->h) continue;
        int yy = miry ? dh - 1 - y : y;
        int SY = sr.top + (int)((int64_t)yy * sh / dh); if (SY < 0 || SY >= s->h) continue;
        for (int x = 0; x < dw; x++) {
            int X = dr.left + x; if (X < 0 || X >= d->w) continue;
            int xx = mirx ? dw - 1 - x : x;
            int SX = sr.left + (int)((int64_t)xx * sw / dw); if (SX < 0 || SX >= s->w) continue;
            uint32_t v = raw_get(s, SX, SY);
            if (keysrc && v >= klo && v <= khi) continue;
            if (keydst) { uint32_t dv = raw_get(d, X, Y); if (dv < dlo || dv > dhi) continue; }
            raw_put(d, X, Y, same ? v : xrgb_to_raw(d, raw_to_xrgb(s, v), 255));
        }
    }
    d->version++;
    return 0;
}

static HRESULT WINAPI dds_Blt(THIS_S, RECT *dr, ddsurf_t *src, RECT *sr, DWORD flags, DDBLTFX *fx) { API_NOTE("dds_Blt");
    ddraw_run_deferred();
    g_dd_caller = __builtin_return_address(0);
    { extern int d3d_is_gpu_target(ddsurf_t *s); 
      if (d3d_is_gpu_target(This) || (src && d3d_is_gpu_target(src)))
        DTRACE("GPU-BLT site=%p dst=%p(%dx%d gpu=%d) src=%p(%dx%d gpu=%d) flags=%x dr=%d,%d,%d,%d sr=%d,%d,%d,%d",
           g_dd_caller, (void *)This, This->w, This->h, d3d_is_gpu_target(This), (void *)src, src ? src->w : 0, src ? src->h : 0, src ? d3d_is_gpu_target(src) : 0, flags,
           dr ? dr->left : -1, dr ? dr->top : -1, dr ? dr->right : -1, dr ? dr->bottom : -1, sr ? sr->left : -1, sr ? sr->top : -1, sr ? sr->right : -1, sr ? sr->bottom : -1); }
    DTRACE("Blt(dst=%p %dx%d, src=%p %dx%d, flags=%x, dr=%d,%d,%d,%d sr=%d,%d,%d,%d)", (void *)This, This->w, This->h, (void *)src, src ? src->w : 0, src ? src->h : 0, flags,
           dr ? dr->left : -1, dr ? dr->top : -1, dr ? dr->right : -1, dr ? dr->bottom : -1, sr ? sr->left : -1, sr ? sr->top : -1, sr ? sr->right : -1, sr ? sr->bottom : -1);
    /* GP3 2000 loads its road detail map (TarmacMap.bmp) into a power-of-two texture
     * with Blt(dst rect = bitmap size) and then tiles it as if the texture were exactly
     * the bitmap: on hardware without the POW2 restriction the texture has the bitmap's
     * size. A bitmap that is not a power of two (e.g. a 133x133 replacement) would fill
     * only a corner and the rest of every tile would be black: scale it to the whole
     * texture instead. Only square images: screen copies (800x600 into 1024x1024 and
     * the like) are drawn with matching texture coordinates and must stay in the corner. */
    RECT full;
    if (src && dr && !sr && (This->caps.caps & DDSCAPS_TEXTURE) && !(flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) &&
        dr->left == 0 && dr->top == 0 && dr->right == src->w && dr->bottom == src->h && src->w == src->h && (src->w < This->w || src->h < This->h)) {
        int pw = 1, ph = 1; while (pw < src->w) pw <<= 1; while (ph < src->h) ph <<= 1;
        if (pw == This->w && ph == This->h && (pw != src->w || ph != src->h)) {
            full = (RECT){ 0, 0, This->w, This->h }; dr = &full;
            static int n; if (n++ < 8) LOG("texture %dx%d filled from a %dx%d image: scaled to the whole texture", This->w, This->h, src->w, src->h);
        }
    }
    DDLOCK();
    HRESULT r = blit(This, dr, src, sr, flags, fx);
    if (This->is_primary) dds_present(This);
    DDUNLOCK();
    return r;
}
static HRESULT WINAPI dds_BltBatch(THIS_S, void *b, DWORD n, DWORD f) { API_NOTE("dds_BltBatch"); (void)This; (void)b; (void)n; (void)f; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI dds_BltFast(THIS_S, DWORD x, DWORD y, ddsurf_t *src, RECT *sr, DWORD trans) { API_NOTE("dds_BltFast");
    ddraw_run_deferred();
    g_dd_caller = __builtin_return_address(0);
    if (!src) return DDERR_INVALIDPARAMS;
    RECT s = sr ? *sr : (RECT){ 0, 0, src->w, src->h };
    RECT d = { (LONG)x, (LONG)y, (LONG)x + s.right - s.left, (LONG)y + s.bottom - s.top };
    DWORD fl = 0;
    if (trans & 1) fl |= DDBLT_KEYSRC;
    if (trans & 2) fl |= DDBLT_KEYDEST;
    DTRACE("BltFast(dst=%p, %u,%u, src=%p %dx%d, trans=%x)", (void *)This, x, y, (void *)src, src->w, src->h, trans);
    DDLOCK();
    HRESULT r = blit(This, &d, src, &s, fl, NULL);
    if (This->is_primary) dds_present(This);
    DDUNLOCK();
    return r;
}
static HRESULT WINAPI dds_DeleteAttachedSurface(THIS_S, DWORD f, ddsurf_t *a) { API_NOTE("dds_DeleteAttachedSurface");
    (void)f;
    if (a && This->zbuf == a) { This->zbuf = NULL; dds_Release(a); return 0; }
    if (!a && This->zbuf) { dds_Release(This->zbuf); This->zbuf = NULL; }
    return 0;
}
typedef HRESULT (WINAPI *enumsurf_cb)(void *s, DDSURFACEDESC2 *d, void *ctx);
static HRESULT WINAPI dds_EnumAttachedSurfaces(THIS_S, void *ctx, enumsurf_cb cb) { API_NOTE("dds_EnumAttachedSurfaces");
    ddsurf_t *list[3] = { This->back, This->zbuf, This->mip_next };
    for (int i = 0; i < 3; i++) {
        ddsurf_t *a = list[i];
        if (!a || a == This) continue;
        DDSURFACEDESC2 d; d.size = 124; dds_fill_desc(a, &d);
        a->refs++;
        if (cb(a, &d, ctx) == 0) break;
    }
    return 0;
}
static HRESULT WINAPI dds_EnumOverlayZOrders(THIS_S, DWORD f, void *c, void *cb) { API_NOTE("dds_EnumOverlayZOrders"); (void)This; (void)f; (void)c; (void)cb; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI dds_Flip(THIS_S, ddsurf_t *target, DWORD flags) { API_NOTE("dds_Flip");
    ddraw_run_deferred();
    (void)target;
    TRACE("Flip(%p, flags=%x)", (void *)This, flags);
    if (!This->back) return DDERR_INVALIDPARAMS;
    DDLOCK();
    /* rotate buffer memory along the chain: front <- back1 <- back2 ... <- front */
    ddsurf_t *chain[8]; int n = 0;
    for (ddsurf_t *s = This; s && n < 8; s = s->back) { chain[n++] = s; if (s->back == This) break; }
    uint8_t *first = chain[0]->bits; void *fg = chain[0]->gpu;
    for (int i = 0; i < n - 1; i++) { chain[i]->bits = chain[i + 1]->bits; chain[i]->gpu = chain[i + 1]->gpu; chain[i]->version++; }
    chain[n - 1]->bits = first; chain[n - 1]->gpu = fg; chain[n - 1]->version++;
    extern void d3d_flip_hook(ddsurf_t **chain, int n);
    d3d_flip_hook(chain, n);
    g_pending_present = NULL;
    dds_present_now(This);
    DDUNLOCK();
    if (!(flags & 8)) wait_vblank();   /* DDFLIP_NOVSYNC */
    return 0;
}
static HRESULT WINAPI dds_GetAttachedSurface(THIS_S, DDSCAPS2 *caps, ddsurf_t **out) { API_NOTE("dds_GetAttachedSurface");
    ddsurf_t *r = NULL;
    if (caps->caps & DDSCAPS_ZBUFFER) r = This->zbuf;
    else if (caps->caps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP)) r = This->back;
    else if (caps->caps & DDSCAPS_MIPMAP) r = This->mip_next;
    else if (caps->caps & DDSCAPS_TEXTURE) r = This->mip_next;
    else r = This->back;
    TRACE("GetAttachedSurface(caps=%x) -> %p", caps->caps, (void *)r);
    if (!r) { *out = NULL; return DDERR_NOTFOUND; }
    r->refs++;
    *out = r;
    return 0;
}
static HRESULT WINAPI dds_GetBltStatus(THIS_S, DWORD f) { API_NOTE("dds_GetBltStatus"); (void)This; (void)f; return 0; }
static HRESULT WINAPI dds_GetCaps(THIS_S, DDSCAPS2 *c) { API_NOTE("dds_GetCaps"); *c = This->caps; return 0; }
static HRESULT WINAPI dds_GetClipper(THIS_S, ddclip_t **c) { API_NOTE("dds_GetClipper"); if (!This->clip) return DDERR_NOCLIPPERATTACHED; This->clip->refs++; *c = This->clip; return 0; }
static HRESULT WINAPI dds_GetColorKey(THIS_S, DWORD f, DDCOLORKEY *k) { API_NOTE("dds_GetColorKey");
    if ((f & 8) && This->has_ck_src) { *k = This->ck_src; return 0; }
    if ((f & 2) && This->has_ck_dst) { *k = This->ck_dst; return 0; }
    return DDERR_NOCOLORKEY;
}
static HRESULT WINAPI dds_GetDC(THIS_S, uint32_t *hdc) { API_NOTE("dds_GetDC");
    g_dd_caller = __builtin_return_address(0);
    d3d_cpu_need(This);
    uint32_t masks[3] = { This->pf.rmask, This->pf.gmask, This->pf.bmask };
    This->hdc = gdi_surface_dc(This->bits, This->w, This->h, This->pitch, This->bpp, This->pal ? This->pal->xrgb : NULL, masks);
    *hdc = This->hdc;
    return 0;
}
static HRESULT WINAPI dds_GetFlipStatus(THIS_S, DWORD f) { API_NOTE("dds_GetFlipStatus"); (void)This; (void)f; return 0; }
static HRESULT WINAPI dds_GetOverlayPosition(THIS_S, LONG *x, LONG *y) { API_NOTE("dds_GetOverlayPosition"); (void)This; (void)x; (void)y; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI dds_GetPalette(THIS_S, ddpal_t **p) { API_NOTE("dds_GetPalette"); if (!This->pal) return DDERR_NOPALETTEATTACHED; This->pal->refs++; *p = This->pal; return 0; }
static HRESULT WINAPI dds_GetPixelFormat(THIS_S, DDPIXELFORMAT *pf) { API_NOTE("dds_GetPixelFormat"); *pf = This->pf; return 0; }
static HRESULT WINAPI dds_GetSurfaceDesc(THIS_S, DDSURFACEDESC2 *d) { API_NOTE("dds_GetSurfaceDesc"); dds_fill_desc(This, d); return 0; }
static HRESULT WINAPI dds_Initialize(THIS_S, void *dd, void *d) { API_NOTE("dds_Initialize"); (void)This; (void)dd; (void)d; return 0x887600F5; }
static HRESULT WINAPI dds_IsLost(THIS_S) { API_NOTE("dds_IsLost"); (void)This; return 0; }
static HRESULT WINAPI dds_Lock(THIS_S, RECT *r, DDSURFACEDESC2 *d, DWORD flags, HANDLE ev) { API_NOTE("dds_Lock");
    ddraw_run_deferred();
    g_dd_caller = __builtin_return_address(0);
    (void)ev; (void)flags;
    extern void d3d_lock_hook(ddsurf_t *s, DWORD flags);
    This->lock_ro = (flags & 0x10) != 0;   /* DDLOCK_READONLY */
    d3d_lock_hook(This, flags);
#ifndef GP3_WIN
    { static int acc = -1; if (acc < 0) acc = getenv("GP3_TRACE_ACC") != NULL;
      extern uint8_t *g_acc_base; extern size_t g_acc_len; extern FILE *g_acc_file;
      static uint32_t acc_site; if (!acc_site) acc_site = getenv("GP3_TRACE_SITE") ? strtoul(getenv("GP3_TRACE_SITE"), NULL, 16) : 0x5148da;
      if (acc && (uint32_t)(uintptr_t)g_dd_caller == acc_site && This->bpp == 16 && !g_acc_base) {
          extern void d3d_cpu_need(ddsurf_t *s); d3d_cpu_need(This);
          if (!g_acc_file) g_acc_file = fopen("out/acc.bin", "wb");
          g_acc_len = (size_t)This->pitch * This->h; g_acc_base = This->bits;
          plat_protect(g_acc_base, g_acc_len, 0);
      } }
#endif
    if (g_diff_site && (uint32_t)(uintptr_t)g_dd_caller == g_diff_site && !g_diff_snap && This->bpp == 16) {
        extern void d3d_cpu_need(ddsurf_t *s); d3d_cpu_need(This);
        static int flip; uint16_t S = getenv("GP3_SENT") ? ((flip++ & 1) ? 0x18e3 : 0x0841) : 0;
        if (S) for (int y = 0; y < This->h; y++) { uint16_t *row = (uint16_t *)(This->bits + (size_t)y * This->pitch); for (int x = 0; x < This->w; x++) row[x] = S; }
        g_diff_snap = malloc((size_t)This->pitch * This->h); memcpy(g_diff_snap, This->bits, (size_t)This->pitch * This->h); g_diff_surf = This;
    }
    dds_fill_desc(This, d);
    uint8_t *p = This->bits;
    { extern uint32_t g_scratch_site; static uint8_t *scratch; static size_t scratch_n;
      if (g_scratch_site && (uint32_t)(uintptr_t)g_dd_caller == g_scratch_site) {
          size_t n = (size_t)This->pitch * This->h;
          if (n > scratch_n) { free(scratch); scratch = calloc(1, n); scratch_n = n; }
          p = scratch; This->lock_ro = 1;   /* discard writes */
      } }
    if (r) p += (ptrdiff_t)r->top * This->pitch + r->left * ((This->bpp + 7) / 8);
    d->surface = (uint32_t)(uintptr_t)p;
    d->flags |= DDSD_LPSURFACE;
    This->lock_count++;
    if (!This->is_primary) DTRACE("Lock(%p %dx%dx%d) -> %p pitch %d caps %x caller %p", (void *)This, This->w, This->h, This->bpp, (void *)p, This->pitch, This->caps.caps, g_dd_caller);
    if ((uint32_t)(uintptr_t)g_dd_caller == 0x516577 && g_log_level >= 2 && gp3_addrs() && gp3_addrs()->fov_site == 0x4082fb) {
        const uint16_t *hdr = *(const uint16_t **)(uintptr_t)0xc943c8;
        LOG("Lock@516577 surf %p %dx%d pitch %d caps %x own %d -> image %ux%u", (void *)This, This->w, This->h, This->pitch, This->caps.caps, This->own_bits, hdr ? hdr[2] : 0, hdr ? hdr[3] : 0);
    }
    return 0;
}
static HRESULT WINAPI dds_ReleaseDC(THIS_S, uint32_t hdc) { API_NOTE("dds_ReleaseDC");
    gdi_free_dc(hdc);
    This->hdc = 0;
    This->version++;
    d3d_cpu_written(This);
    if (This->is_primary) { DDLOCK(); dds_present(This); DDUNLOCK(); }
    return 0;
}
static HRESULT WINAPI dds_Restore(THIS_S) { API_NOTE("dds_Restore"); (void)This; return 0; }
static HRESULT WINAPI dds_SetClipper(THIS_S, ddclip_t *c) { API_NOTE("dds_SetClipper");
    if (This->clip) This->clip->refs--;
    This->clip = c; if (c) c->refs++;
    return 0;
}
static HRESULT WINAPI dds_SetColorKey(THIS_S, DWORD f, DDCOLORKEY *k) { API_NOTE("dds_SetColorKey");
    DTRACE("SetColorKey(%p, f=%x, %x-%x)", (void *)This, f, k ? k->lo : 0, k ? k->hi : 0);
    if (f & 8) { if (k) { This->ck_src = *k; This->has_ck_src = 1; } else This->has_ck_src = 0; }
    if (f & 2) { if (k) { This->ck_dst = *k; This->has_ck_dst = 1; } else This->has_ck_dst = 0; }
    This->version++;
    return 0;
}
static HRESULT WINAPI dds_SetOverlayPosition(THIS_S, LONG x, LONG y) { API_NOTE("dds_SetOverlayPosition"); (void)This; (void)x; (void)y; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI dds_SetPalette(THIS_S, ddpal_t *p) { API_NOTE("dds_SetPalette");
    if (p) p->refs++;
    if (This->pal) This->pal->refs--;
    This->pal = p;
    /* the palette of the primary also applies to its flip chain */
    if (This->is_primary) for (ddsurf_t *b = This->back; b && b != This; b = b->back) b->pal = p;
    This->version++;
    if (This->is_primary && p) { DDLOCK(); dds_present(This); DDUNLOCK(); }
    return 0;
}
int ddraw_diff_overlay(uint32_t *buf, int w, int h);
/* diagnostics: highlight pixels written by the CPU between Lock/Unlock from a given game call site */
void ddraw_diff_resnap(ddsurf_t *s) {
    if (g_diff_surf == s && g_diff_snap) memcpy(g_diff_snap, s->bits, (size_t)s->pitch * s->h);
}
int ddraw_diff_overlay(uint32_t *buf, int w, int h) {
    if (!g_diff_acc || g_diff_w != w || g_diff_h != h) return 0;
    for (int i = 0; i < w * h; i++) if (g_diff_acc[i]) buf[i] = 0xff0000;
    memset(g_diff_acc, 0, (size_t)w * h * 4);
    return 1;
}
static HRESULT WINAPI dds_Unlock(THIS_S, RECT *r) { API_NOTE("dds_Unlock");
    (void)r;
#ifndef GP3_WIN
    { extern uint8_t *g_acc_base; extern size_t g_acc_len; extern FILE *g_acc_file;
      if (g_acc_base == This->bits) {
          plat_protect(g_acc_base, g_acc_len, 2); g_acc_base = NULL;
          if (g_acc_file) { uint32_t rec[4] = { 0xfffffffeu, 0, 3, 0 }; fwrite(rec, 4, 4, g_acc_file); fflush(g_acc_file); }
      } }
#endif
    if (g_diff_surf == This && g_diff_snap) {
        if (!g_diff_acc || g_diff_w != This->w || g_diff_h != This->h) { free(g_diff_acc); g_diff_acc = calloc((size_t)This->w * This->h, 4); g_diff_w = This->w; g_diff_h = This->h; }
        int Bpp = (This->bpp + 7) / 8;
        static FILE *pairs; static long npairs;
        if (!pairs && Bpp == 2) pairs = fopen("out/diff_pairs.bin", "wb");
        if (pairs && npairs < 4000000) for (int y = 0; y < This->h; y++) for (int x = 0; x < This->w; x++) {
            uint16_t b = *(uint16_t *)(g_diff_snap + (size_t)y * This->pitch + x * 2), a = *(uint16_t *)(This->bits + (size_t)y * This->pitch + x * 2);
            if (a != b) { uint16_t pr[2] = { b, a }; fwrite(pr, 2, 2, pairs); npairs++; }
        }
        if (pairs) fflush(pairs);
        for (int y = 0; y < This->h; y++) for (int x = 0; x < This->w; x++)
            if (memcmp(g_diff_snap + (size_t)y * This->pitch + x * Bpp, This->bits + (size_t)y * This->pitch + x * Bpp, Bpp)) g_diff_acc[y * This->w + x] = 0xffff0000u | (++g_diff_n & 0xff);
        free(g_diff_snap); g_diff_snap = NULL; g_diff_surf = NULL;
    }
    if (This->lock_count > 0) This->lock_count--;
    extern void d3d_unlock_hook(ddsurf_t *s, int readonly);
    if (!This->lock_ro) This->version++;
    d3d_unlock_hook(This, This->lock_ro);
    if (This->lock_ro) { This->lock_ro = 0; return 0; }
    if (This->is_primary) { DDLOCK(); dds_present(This); DDUNLOCK(); }
    return 0;
}
static HRESULT WINAPI dds_UpdateOverlay(THIS_S, RECT *a, void *b, RECT *c, DWORD f, void *fx) { API_NOTE("dds_UpdateOverlay"); (void)This; (void)a; (void)b; (void)c; (void)f; (void)fx; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI dds_UpdateOverlayDisplay(THIS_S, DWORD f) { API_NOTE("dds_UpdateOverlayDisplay"); (void)This; (void)f; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI dds_UpdateOverlayZOrder(THIS_S, DWORD f, void *r) { API_NOTE("dds_UpdateOverlayZOrder"); (void)This; (void)f; (void)r; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI dds_GetDDInterface(THIS_S, void **dd) { API_NOTE("dds_GetDDInterface"); *dd = This->dd; This->dd->refs++; return 0; }
static HRESULT WINAPI dds_PageLock(THIS_S, DWORD f) { API_NOTE("dds_PageLock"); (void)This; (void)f; return 0; }
static HRESULT WINAPI dds_PageUnlock(THIS_S, DWORD f) { API_NOTE("dds_PageUnlock"); (void)This; (void)f; return 0; }
static HRESULT WINAPI dds_SetSurfaceDesc(THIS_S, DDSURFACEDESC2 *d, DWORD f) { API_NOTE("dds_SetSurfaceDesc");
    (void)f;
    if (d->flags & DDSD_LPSURFACE) {
        if (This->own_bits) surf_mem_free(This->bits, surf_alloc_size(This->pitch, This->h));
        This->bits = (uint8_t *)(uintptr_t)d->surface; This->own_bits = 0;
        if (d->flags & DDSD_PITCH) This->pitch = d->pitch;
        if (d->flags & DDSD_WIDTH) This->w = d->width;
        if (d->flags & DDSD_HEIGHT) This->h = d->height;
        if (d->flags & DDSD_PIXELFORMAT) { This->pf = d->pf; This->bpp = d->pf.bpp; }
        This->version++;
    }
    return 0;
}
static HRESULT WINAPI dds_SetPrivateData(THIS_S, const uint8_t *g, void *d, DWORD n, DWORD f) { API_NOTE("dds_SetPrivateData"); (void)This; (void)g; (void)d; (void)n; (void)f; return 0; }
static HRESULT WINAPI dds_GetPrivateData(THIS_S, const uint8_t *g, void *d, DWORD *n) { API_NOTE("dds_GetPrivateData"); (void)This; (void)g; (void)d; (void)n; return DDERR_NOTFOUND; }
static HRESULT WINAPI dds_FreePrivateData(THIS_S, const uint8_t *g) { API_NOTE("dds_FreePrivateData"); (void)This; (void)g; return 0; }
static HRESULT WINAPI dds_GetUniquenessValue(THIS_S, DWORD *v) { API_NOTE("dds_GetUniquenessValue"); *v = This->version; return 0; }
static HRESULT WINAPI dds_ChangeUniquenessValue(THIS_S) { API_NOTE("dds_ChangeUniquenessValue"); This->version++; return 0; }
static HRESULT WINAPI dds_SetPriority(THIS_S, DWORD p) { API_NOTE("dds_SetPriority"); This->priority = p; return 0; }
static HRESULT WINAPI dds_GetPriority(THIS_S, DWORD *p) { API_NOTE("dds_GetPriority"); *p = This->priority; return 0; }
static HRESULT WINAPI dds_SetLOD(THIS_S, DWORD l) { API_NOTE("dds_SetLOD"); This->lod = l; return 0; }
static HRESULT WINAPI dds_GetLOD(THIS_S, DWORD *l) { API_NOTE("dds_GetLOD"); *l = This->lod; return 0; }

/* ------------------------------------------------------------------ */
/* IDirectDraw7                                                         */
static HRESULT WINAPI dd_QueryInterface(THIS_DD, const uint8_t *iid, void **out) { API_NOTE("dd_QueryInterface");
    char g[64]; guid_str(iid, g);
    if (guid_eq(iid, IID_IDirectDraw7) || guid_eq(iid, IID_IDirectDraw4) || guid_eq(iid, IID_IUnknown)) {
        *out = This; This->refs++; return 0;
    }
    if (guid_eq(iid, IID_IDirect3D7)) {
        HRESULT r = d3d_create(This, out);
        LOG("DirectDraw::QueryInterface(IDirect3D7) -> %x", r);
        return r;
    }
    LOG("DirectDraw::QueryInterface(%s) -> E_NOINTERFACE", g);
    *out = NULL;
    return E_NOINTERFACE;
}
static DWORD WINAPI dd_AddRef(THIS_DD) { API_NOTE("dd_AddRef"); return ++This->refs; }
static DWORD WINAPI dd_Release(THIS_DD) { API_NOTE("dd_Release"); DWORD r = --This->refs; if (!r) { LOG("DirectDraw released"); if (g_ddraw == This) g_ddraw = NULL; free(This); } return r; }
static HRESULT WINAPI dd_Compact(THIS_DD) { API_NOTE("dd_Compact"); (void)This; return 0; }
static HRESULT WINAPI dd_CreateClipper(THIS_DD, DWORD f, ddclip_t **out, void *outer) { API_NOTE("dd_CreateClipper");
    (void)This; (void)f; (void)outer;
    ddclip_t *c = calloc(1, sizeof *c); c->vtbl = g_clip_vtbl; c->refs = 1; *out = c; return 0;
}
static HRESULT WINAPI dd_CreatePalette(THIS_DD, DWORD f, const uint8_t *entries, ddpal_t **out, void *outer) { API_NOTE("dd_CreatePalette");
    (void)This; (void)outer;
    ddpal_t *p = calloc(1, sizeof *p); p->vtbl = g_pal_vtbl; p->refs = 1; p->caps = f;
    if (entries) pal_set(p, 0, (f & 4) ? 256 : 16, entries);
    *out = p;
    DTRACE("CreatePalette(f=%x) -> %p", f, (void *)p);
    return 0;
}
static HRESULT WINAPI dd_CreateSurface(THIS_DD, DDSURFACEDESC2 *d, ddsurf_t **out, void *outer) { API_NOTE("dd_CreateSurface");
    ddraw_run_deferred();
    (void)outer;
    DDLOCK();
    ddsurf_t *s = dds_create(This, d);
    DDUNLOCK();
    LOG("CreateSurface(flags=%x caps=%x/%x %ux%u bpp=%u pf=%x back=%u) -> %p",
        d->flags, d->caps.caps, d->caps.caps2, d->width, d->height, (d->flags & DDSD_PIXELFORMAT) ? d->pf.bpp : 0,
        (d->flags & DDSD_PIXELFORMAT) ? d->pf.flags : 0, (d->flags & DDSD_BACKBUFFERCOUNT) ? d->backbuffers : 0, (void *)s);
    if (!s) return DDERR_INVALIDPARAMS;
    *out = s;
    return 0;
}
static HRESULT WINAPI dd_DuplicateSurface(THIS_DD, ddsurf_t *s, ddsurf_t **o) { API_NOTE("dd_DuplicateSurface"); (void)This; (void)s; (void)o; return DDERR_UNSUPPORTED; }

static int g_modes[][2] = { {640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 960}, {1280, 1024}, {1600, 1200}, {0, 0} };
void ddraw_add_mode(int w, int h) { g_modes[7][0] = w; g_modes[7][1] = h; }   /* widescreen mode (last slot) */
typedef HRESULT (WINAPI *enummode_cb)(DDSURFACEDESC2 *, void *);
static HRESULT WINAPI dd_EnumDisplayModes(THIS_DD, DWORD f, DDSURFACEDESC2 *filter, void *ctx, enummode_cb cb) { API_NOTE("dd_EnumDisplayModes");
    (void)This; (void)f;
    static const int bpps[] = { 8, 16, 32 };
    for (int b = 0; b < 3; b++)
        for (size_t m = 0; m < sizeof g_modes / sizeof g_modes[0]; m++) {
            if (!g_modes[m][0]) continue;
            DDSURFACEDESC2 d; memset(&d, 0, sizeof d);
            d.size = 124; d.flags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            d.width = g_modes[m][0]; d.height = g_modes[m][1]; d.pitch = d.width * bpps[b] / 8; d.mipmaps = 60;
            pf_from_bpp(&d.pf, bpps[b]);
            if (filter && (filter->flags & DDSD_WIDTH) && filter->width != d.width) continue;
            if (filter && (filter->flags & DDSD_HEIGHT) && filter->height != d.height) continue;
            if (filter && (filter->flags & DDSD_PIXELFORMAT) && filter->pf.bpp != d.pf.bpp) continue;
            if (cb(&d, ctx) == 0) return 0;
        }
    return 0;
}
static HRESULT WINAPI dd_EnumSurfaces(THIS_DD, DWORD f, void *d, void *c, void *cb) { API_NOTE("dd_EnumSurfaces"); (void)This; (void)f; (void)d; (void)c; (void)cb; return 0; }
static HRESULT WINAPI dd_FlipToGDISurface(THIS_DD) { API_NOTE("dd_FlipToGDISurface"); (void)This; return 0; }
static HRESULT WINAPI dd_GetCaps(THIS_DD, uint8_t *hal, uint8_t *hel) { API_NOTE("dd_GetCaps");
    (void)This;
    uint8_t *c[2] = { hal, hel };
    for (int i = 0; i < 2; i++) {
        if (!c[i]) continue;
        DWORD sz = *(DWORD *)c[i];
        memset(c[i] + 4, 0, sz - 4);
        DWORD *d = (DWORD *)c[i];
        d[1] = 0x00000040 | 0x00000200 | 0x00000400 | 0x00001000 | 0x00004000 | 0x00008000 | 0x04000000 | 0x80000000; /* caps: BLT, BLTCOLORFILL, BLTSTRETCH, COLORKEY, 3D, PALETTE, ... */
        d[2] = 0x00002000 | 0x00080000 | 0x00000200 | 0x00010000; /* caps2: CANRENDERWINDOWED, WIDESURFACES, PRIMARYGAMMA, ... */
        d[3] = 0x00000200 | 0x00000001; /* CKeyCaps: SRCBLT */
        d[4] = 0x00000002 | 0x00000004; /* FX caps: mirror */
        d[6] = 0x00000004 | 0x00000040; /* palette caps */
        d[15] = 256u << 20; d[16] = 224u << 20;   /* vid mem total/free */
        if (sz >= 384) { DWORD *caps = (DWORD *)(c[i] + 0x68 - 0); (void)caps; }
        /* ddsCaps (legacy DDSCAPS at offset 0x68 in DDCAPS_DX7 layout) */
        if (sz >= 0x6c + 4) *(DWORD *)(c[i] + 0x68) = DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE | DDSCAPS_ZBUFFER | DDSCAPS_FLIP | DDSCAPS_PRIMARYSURFACE |
                                                      DDSCAPS_BACKBUFFER | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_MIPMAP | DDSCAPS_PALETTE;
    }
    return 0;
}
static HRESULT WINAPI dd_GetDisplayMode(THIS_DD, DDSURFACEDESC2 *d) { API_NOTE("dd_GetDisplayMode");
    DWORD sz = d->size == 108 ? 108 : 124;
    memset(d, 0, sz); d->size = sz;
    d->flags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    int w = This->mode_w, h = This->mode_h, b = This->mode_bpp;
    if (!This->coop || !(This->coop & 0x10)) { /* not exclusive: report the "desktop" */ }
    d->width = w; d->height = h; d->pitch = w * b / 8; d->mipmaps = 60;
    pf_from_bpp(&d->pf, b);
    return 0;
}
static HRESULT WINAPI dd_GetFourCCCodes(THIS_DD, DWORD *n, DWORD *codes) { API_NOTE("dd_GetFourCCCodes"); (void)This; (void)codes; *n = 0; return 0; }
static HRESULT WINAPI dd_GetGDISurface(THIS_DD, ddsurf_t **s) { API_NOTE("dd_GetGDISurface"); if (!This->primary) return DDERR_NOTFOUND; This->primary->refs++; *s = This->primary; return 0; }
static HRESULT WINAPI dd_GetMonitorFrequency(THIS_DD, DWORD *f) { API_NOTE("dd_GetMonitorFrequency"); (void)This; *f = 60; return 0; }
static HRESULT WINAPI dd_GetScanLine(THIS_DD, DWORD *l) { API_NOTE("dd_GetScanLine"); (void)This; *l = (DWORD)((rt_now_us() % 16667) * 525 / 16667); return 0; }
static HRESULT WINAPI dd_GetVerticalBlankStatus(THIS_DD, BOOL *b) { API_NOTE("dd_GetVerticalBlankStatus"); (void)This; *b = (rt_now_us() % 16667) < 1000; return 0; }
static HRESULT WINAPI dd_Initialize(THIS_DD, void *g) { API_NOTE("dd_Initialize"); (void)This; (void)g; return 0x887600F5; }
static HRESULT WINAPI dd_RestoreDisplayMode(THIS_DD) { API_NOTE("dd_RestoreDisplayMode"); (void)This; return 0; }
static HRESULT WINAPI dd_SetCooperativeLevel(THIS_DD, HWND h, DWORD f) { API_NOTE("dd_SetCooperativeLevel");
    LOG("SetCooperativeLevel(hwnd=%x, flags=%x)", (unsigned)(uintptr_t)h, f);
    This->hwnd = h; This->coop = f;
    plat_set_window((void *)(uintptr_t)h, f);
    return 0;
}
static HRESULT WINAPI dd_SetDisplayMode(THIS_DD, DWORD w, DWORD h, DWORD bpp, DWORD rate, DWORD f) { API_NOTE("dd_SetDisplayMode");
    LOG("SetDisplayMode(%ux%ux%u @%u, f=%x)", w, h, bpp, rate, f);
    This->mode_w = w; This->mode_h = h; This->mode_bpp = bpp;
    plat_set_mode(w, h, bpp);
    return 0;
}
static HRESULT WINAPI dd_WaitForVerticalBlank(THIS_DD, DWORD f, HANDLE e) { API_NOTE("dd_WaitForVerticalBlank"); (void)This; (void)e; if (f == 1 || f == 4) wait_vblank(); return 0; }
static HRESULT WINAPI dd_GetAvailableVidMem(THIS_DD, DDSCAPS2 *c, DWORD *total, DWORD *freem) { API_NOTE("dd_GetAvailableVidMem");
    (void)This; (void)c;
    if (total) *total = 256u << 20;
    if (freem) *freem = 224u << 20;
    return 0;
}
static HRESULT WINAPI dd_GetSurfaceFromDC(THIS_DD, uint32_t dc, ddsurf_t **s) { API_NOTE("dd_GetSurfaceFromDC"); (void)This; (void)dc; *s = NULL; return DDERR_NOTFOUND; }
static HRESULT WINAPI dd_RestoreAllSurfaces(THIS_DD) { API_NOTE("dd_RestoreAllSurfaces"); (void)This; return 0; }
static HRESULT WINAPI dd_TestCooperativeLevel(THIS_DD) { API_NOTE("dd_TestCooperativeLevel"); (void)This; return 0; }
static HRESULT WINAPI dd_GetDeviceIdentifier(THIS_DD, uint8_t *id, DWORD f) { API_NOTE("dd_GetDeviceIdentifier");
    (void)This; (void)f;
    memset(id, 0, 1024 + 8 + 16 + 16 + 4);
    snprintf((char *)id, 512, "gp3rt.dll");
    snprintf((char *)id + 512, 512, "GP3 Native Renderer (Direct3D 7 compatible)");
    *(uint64_t *)(id + 1024) = 0x0006000E000B0001ull;   /* 6.14.11.1 */
    *(DWORD *)(id + 1032) = 0x1414;  /* vendor: no device-specific workarounds in GP3 */
    *(DWORD *)(id + 1036) = 0x0001;
    *(DWORD *)(id + 1040) = 0;
    *(DWORD *)(id + 1044) = 1;
    static const uint8_t g[16] = { 0x47, 0x50, 0x33, 0x52, 0x54, 0x00, 0x01, 0x00, 0x8a, 0x5f, 0x19, 0x98, 0x07, 0x01, 0x20, 0x00 };
    memcpy(id + 1048, g, 16);
    *(DWORD *)(id + 1064) = 1;
    return 0;
}
static HRESULT WINAPI dd_StartModeTest(THIS_DD, void *m, DWORD n, DWORD f) { API_NOTE("dd_StartModeTest"); (void)This; (void)m; (void)n; (void)f; return DDERR_UNSUPPORTED; }
static HRESULT WINAPI dd_EvaluateMode(THIS_DD, DWORD f, DWORD *t) { API_NOTE("dd_EvaluateMode"); (void)This; (void)f; (void)t; return DDERR_UNSUPPORTED; }

/* ------------------------------------------------------------------ */
/* exports                                                              */
typedef BOOL (WINAPI *ddenum_cb)(const uint8_t *guid, const char *desc, const char *name, void *ctx, uint32_t hmon);
static HRESULT WINAPI DirectDrawEnumerateExA_(ddenum_cb cb, void *ctx, DWORD flags) {
    LOG("DirectDrawEnumerateExA(flags=%x)", flags);
    cb(NULL, "Primary Display Driver", "display", ctx, 0);
    return 0;
}
static HRESULT WINAPI DirectDrawEnumerateA_(void *cb, void *ctx) {
    typedef BOOL (WINAPI *f)(const uint8_t *, const char *, const char *, void *);
    ((f)cb)(NULL, "Primary Display Driver", "display", ctx);
    return 0;
}
static HRESULT WINAPI DirectDrawCreateEx_(const uint8_t *guid, void **out, const uint8_t *iid, void *outer) {
    (void)guid; (void)outer;
    if (!guid_eq(iid, IID_IDirectDraw7)) { char g[64]; guid_str(iid, g); WARN("DirectDrawCreateEx: iid %s", g); }
    ddraw_t *d = calloc(1, sizeof *d);
    d->vtbl = g_dd7_vtbl; d->refs = 1;
    d->mode_w = 640; d->mode_h = 480; d->mode_bpp = 32;   /* the "desktop" */
    g_ddraw = d;
    *out = d;
    LOG("DirectDrawCreateEx -> %p", (void *)d);
    return 0;
}

extern void d3d_register(void);
void ddraw_register(void) {
    d3d_register();
    os_mutex_init(&g_dd_lock);

    void *dd[] = { dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
                   dd_DuplicateSurface, dd_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface, dd_GetCaps, dd_GetDisplayMode,
                   dd_GetFourCCCodes, dd_GetGDISurface, dd_GetMonitorFrequency, dd_GetScanLine, dd_GetVerticalBlankStatus,
                   dd_Initialize, dd_RestoreDisplayMode, dd_SetCooperativeLevel, dd_SetDisplayMode, dd_WaitForVerticalBlank,
                   dd_GetAvailableVidMem, dd_GetSurfaceFromDC, dd_RestoreAllSurfaces, dd_TestCooperativeLevel,
                   dd_GetDeviceIdentifier, dd_StartModeTest, dd_EvaluateMode };
    g_dd7_vtbl = vtbl_alloc(sizeof dd / sizeof dd[0]); memcpy(g_dd7_vtbl, dd, sizeof dd);
    void *ds[] = { dds_QueryInterface, dds_AddRef, dds_Release, dds_AddAttachedSurface, dds_AddOverlayDirtyRect, dds_Blt,
                   dds_BltBatch, dds_BltFast, dds_DeleteAttachedSurface, dds_EnumAttachedSurfaces, dds_EnumOverlayZOrders,
                   dds_Flip, dds_GetAttachedSurface, dds_GetBltStatus, dds_GetCaps, dds_GetClipper, dds_GetColorKey, dds_GetDC,
                   dds_GetFlipStatus, dds_GetOverlayPosition, dds_GetPalette, dds_GetPixelFormat, dds_GetSurfaceDesc,
                   dds_Initialize, dds_IsLost, dds_Lock, dds_ReleaseDC, dds_Restore, dds_SetClipper, dds_SetColorKey,
                   dds_SetOverlayPosition, dds_SetPalette, dds_Unlock, dds_UpdateOverlay, dds_UpdateOverlayDisplay,
                   dds_UpdateOverlayZOrder, dds_GetDDInterface, dds_PageLock, dds_PageUnlock, dds_SetSurfaceDesc,
                   dds_SetPrivateData, dds_GetPrivateData, dds_FreePrivateData, dds_GetUniquenessValue,
                   dds_ChangeUniquenessValue, dds_SetPriority, dds_GetPriority, dds_SetLOD, dds_GetLOD };
    g_dds7_vtbl = vtbl_alloc(sizeof ds / sizeof ds[0]); memcpy(g_dds7_vtbl, ds, sizeof ds);
    void *pl[] = { pal_QueryInterface, pal_AddRef, pal_Release, pal_GetCaps, pal_GetEntries, pal_Initialize, pal_SetEntries };
    g_pal_vtbl = vtbl_alloc(7); memcpy(g_pal_vtbl, pl, sizeof pl);
    void *cl[] = { clip_QueryInterface, clip_AddRef, clip_Release, clip_GetClipList, clip_GetHWnd, clip_Initialize,
                   clip_IsClipListChanged, clip_SetClipList, clip_SetHWnd };
    g_clip_vtbl = vtbl_alloc(9); memcpy(g_clip_vtbl, cl, sizeof cl);
    void *gm[] = { gam_QueryInterface, gam_AddRef, gam_Release, gam_GetGammaRamp, gam_SetGammaRamp };
    g_gamma_vtbl = vtbl_alloc(5); memcpy(g_gamma_vtbl, gm, sizeof gm);

    static const export_t ex[] = {
        { "ddraw", "DirectDrawCreateEx", 0, (void *)DirectDrawCreateEx_ },
        { "ddraw", "DirectDrawEnumerateExA", 0, (void *)DirectDrawEnumerateExA_ },
        { "ddraw", "DirectDrawEnumerateA", 0, (void *)DirectDrawEnumerateA_ },
        { NULL, NULL, 0, NULL } };
    rt_register_exports(ex);
}
