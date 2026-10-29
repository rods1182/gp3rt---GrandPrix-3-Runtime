/* Direct3D 7 implementation (IDirect3D7, IDirect3DDevice7, IDirect3DVertexBuffer7).
 * Draw calls are decoded (FVF) into clip-space vertices and handed to the
 * renderer backend together with a snapshot of the fixed-function state.
 * Surfaces track where their latest contents live (CPU memory and/or the
 * backend render target) so that DirectDraw locks and 3D rendering can be
 * freely mixed, exactly like GP3 does for mirrors and the cockpit. */
#include "ddraw_int.h"
#include "apinote.h"
#include "rdr.h"
#include "plat.h"
#include <math.h>

rbackend_t *g_rdr;
void rdr_init(void);
unsigned g_rdr_tid; static unsigned g_rdr_other_tid;
static rbackend_t *rdr_get(void);
#define RDR rdr_get()
int g_dump_textures;
ddsurf_t *g_render_target;
int g_render_scale = 1;

#define IID_IDirect3DHALDevice    "84E63DE0-46AA-11CF-816F-0000C020156E"
#define IID_IDirect3DTnLHalDevice "F5049E78-4861-11D2-A407-00A0C90629A8"
#define IID_IDirect3DRGBDevice    "A4665C60-2673-11CF-A31A-00AA00B93356"
#define IID_IDirect3DRefDevice    "50936643-13E9-11D1-89AA-00A0C9054129"
#define IID_IDirect3DMMXDevice    "881949A1-D6F3-11D0-89AB-00A0C9054129"

/* ------------------------------------------------------------------ */
/* per-surface renderer data                                            */
typedef struct sx {
    rtex_t *tex; uint32_t tex_ver; int tex_w, tex_h, tex_levels;
    rtarget_t *tgt;
    int cpu_valid, gpu_valid;      /* location of the latest colour data */
    uint8_t *dirty; int dirty_h, ndirty;   /* rows modified on the CPU, not yet uploaded */
    uint8_t *rowok;                        /* rows whose CPU copy is current (when !cpu_valid) */
    uint8_t *ovl; int novl;                /* overlay rows: CPU memory holds key + new pixels, not yet composited */
    uint64_t tex_hash;                     /* content of the last texture upload */
} sx_t;

static sx_t *sx_of(ddsurf_t *s) {
    if (!s->gpu) s->gpu = calloc(1, sizeof(sx_t)), ((sx_t *)s->gpu)->cpu_valid = 1;
    return s->gpu;
}

/* argb <-> surface memory */
static inline uint32_t chan8(uint32_t v, uint32_t m) {
    if (!m) return 0;
    int sh = __builtin_ctz(m), bits = __builtin_popcount(m);
    uint32_t x = (v & m) >> sh;
    if (bits >= 8) return x >> (bits - 8);
    if (bits == 1) return x ? 255 : 0;
    return (x << (8 - bits)) | (x >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0));
}
static inline uint32_t pack8(uint32_t c, uint32_t m) {
    if (!m) return 0;
    int sh = __builtin_ctz(m), bits = __builtin_popcount(m);
    uint32_t x = bits >= 8 ? c << (bits - 8) : c >> (8 - bits);
    return (x << sh) & m;
}
static void surf_to_argb(ddsurf_t *s, uint32_t *out, int ck) {
    uint32_t klo = s->ck_src.lo, khi = s->ck_src.hi < s->ck_src.lo ? s->ck_src.lo : s->ck_src.hi;
    for (int y = 0; y < s->h; y++) {
        uint8_t *row = s->bits + (ptrdiff_t)y * s->pitch;
        for (int x = 0; x < s->w; x++) {
            uint32_t v, c;
            switch (s->bpp) {
            case 8: v = row[x]; c = (s->pal ? s->pal->xrgb[v] : v * 0x010101u) | 0xff000000u; break;
            case 16: v = ((uint16_t *)row)[x]; break;
            case 24: v = row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16; break;
            default: v = ((uint32_t *)row)[x]; break;
            }
            if (s->bpp != 8) {
                uint32_t a = (s->pf.flags & DDPF_ALPHAPIXELS) && s->pf.amask ? chan8(v, s->pf.amask) : 255;
                c = a << 24 | chan8(v, s->pf.rmask) << 16 | chan8(v, s->pf.gmask) << 8 | chan8(v, s->pf.bmask);
            }
            if (ck && v >= klo && v <= khi) c &= 0x00ffffffu;
            *out++ = c;
        }
    }
}
static void argb_to_surf(ddsurf_t *s, const uint32_t *in) {
    for (int y = 0; y < s->h; y++) {
        uint8_t *row = s->bits + (ptrdiff_t)y * s->pitch;
        for (int x = 0; x < s->w; x++) {
            uint32_t c = *in++;
            if (s->bpp == 16 && s->pf.gmask == 0x7e0) { ((uint16_t *)row)[x] = (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x7e0) | ((c >> 3) & 0x1f)); continue; }
            uint32_t v = pack8((c >> 16) & 255, s->pf.rmask) | pack8((c >> 8) & 255, s->pf.gmask) | pack8(c & 255, s->pf.bmask) | pack8(c >> 24, s->pf.amask);
            switch (s->bpp) {
            case 16: ((uint16_t *)row)[x] = (uint16_t)v; break;
            case 24: row[x * 3] = v; row[x * 3 + 1] = v >> 8; row[x * 3 + 2] = v >> 16; break;
            case 32: ((uint32_t *)row)[x] = v; break;
            case 8: row[x] = 0; break;
            }
        }
    }
}

static uint32_t g_guard_skipped, g_rows_loaded, g_rows_uploaded;
uint32_t g_ctx_moves;
static uint32_t g_gpu_copies;
static uint64_t g_ovl_rows, g_ovl_px; static uint32_t g_tex_same; static uint32_t g_shadow_traps;   /* render context hand-overs between threads (platform) */
static uint64_t g_t_read, g_t_upload, g_t_draw, g_t_tex, g_t_present, g_t_lock;
static uint64_t g_t_ovl_gl; static uint32_t g_ovl_flushes; static uint64_t g_t_deferred, g_t_lockwait, g_t_gpuneed, g_t_prep, g_t_list, g_t_tail;
uint32_t g_present_calls;   /* dds_present_now calls (flips + coalesced CPU presents) */
uint64_t g_t_front;  /* whole DrawPrimitive path (state, vertices, renderer) */
uint64_t g_t_ctx;   /* platform: time spent switching the render context (wglMakeCurrent) */   /* microseconds (stats) */
/* statistics (logged periodically) */
static struct { uint32_t downloads, uploads, tex_uploads, tex_bytes, draws, presents; uint64_t t0; } g_st;
#ifdef GP3_WIN
extern uint32_t g_rdr_batches;
static double rdr_batches_take(void) { uint32_t n = g_rdr_batches; g_rdr_batches = 0; return n; }
#else
static double rdr_batches_take(void) { return 0; }
#endif
static void *g_rb_site[16]; static uint32_t g_rb_rows[16];   /* read-back rows per game call site (stats) */
static void stats_tick(void) {
    uint64_t now = rt_now_us();
    if (!g_st.t0) g_st.t0 = now;
    if (now - g_st.t0 < 5000000) return;
    double sec = (now - g_st.t0) / 1e6;
    LOG("stats/s: presents %.1f draws %.0f full-downloads %.1f upload-batches %.1f rows-read %.0f rows-written %.0f tex-uploads %.1f (%.1f MB) untouched-locks %.1f context-moves %.1f tex-unchanged %.0f overlay-mpx %.2f gpu-copies %.0f gl-batches %.0f overlay-rows %.0f shadow-traps %.0f | ms/s read %.0f upload %.0f draw %.0f tex %.0f present %.0f lock %.0f ctx %.0f api-draw %.0f upload-gl %.0f (in draws: deferred %.0f lockwait %.0f gpuneed %.0f prep %.0f list %.0f tail %.0f) | overlay flushes/s %.0f present-calls/s %.0f",
        g_st.presents / sec, g_st.draws / sec, g_st.downloads / sec, g_st.uploads / sec, g_rows_loaded / sec, g_rows_uploaded / sec,
        g_st.tex_uploads / sec, g_st.tex_bytes / sec / 1e6, g_guard_skipped / sec, g_ctx_moves / sec, g_tex_same / sec, g_ovl_px / sec / 1e6, g_gpu_copies / sec, rdr_batches_take() / sec, g_ovl_rows / sec, g_shadow_traps / sec,
        g_t_read / sec / 1000, g_t_upload / sec / 1000, g_t_draw / sec / 1000, g_t_tex / sec / 1000, g_t_present / sec / 1000, g_t_lock / sec / 1000, g_t_ctx / sec / 1000, g_t_front / sec / 1000, g_t_ovl_gl / sec / 1000, g_t_deferred / sec / 1000, g_t_lockwait / sec / 1000, g_t_gpuneed / sec / 1000, g_t_prep / sec / 1000, g_t_list / sec / 1000, g_t_tail / sec / 1000, g_ovl_flushes / sec, g_present_calls / sec);
    if (g_rb_site[0]) {
        char b[256]; int o = 0;
        for (int k = 0; k < 16 && g_rb_site[k]; k++) if (g_rb_rows[k]) o += snprintf(b + o, sizeof b - o, " %p:%u", g_rb_site[k], g_rb_rows[k]);
        if (o) LOG("read-back rows by game call site:%s", b);
        memset(g_rb_rows, 0, sizeof g_rb_rows);
    }
    g_guard_skipped = 0; g_ctx_moves = 0; g_gpu_copies = 0; g_ovl_rows = 0; g_ovl_px = 0; g_tex_same = 0; g_shadow_traps = 0; g_t_read = g_t_upload = g_t_draw = g_t_tex = g_t_present = g_t_lock = 0; g_t_ctx = 0; g_t_front = 0; g_t_ovl_gl = 0; g_ovl_flushes = 0; g_t_deferred = g_t_lockwait = g_t_gpuneed = g_t_prep = g_t_list = g_t_tail = 0; g_present_calls = 0; g_rows_loaded = 0; g_rows_uploaded = 0;
    memset(&g_st, 0, sizeof g_st); g_st.t0 = now;
}
/* ------------------------------------------------------------------ */
/* Render-target locations and lazy readback.
 *
 * A render target's latest image may live on the GPU, in CPU memory, or be
 * split by rows (rows written by the CPU and not uploaded yet are "dirty").
 * GP3 locks its back buffer ~10 times per frame for a CPU span renderer that
 * usually touches only a few rows. Instead of a full GPU->CPU->GPU round trip
 * per Lock, the buffer memory is protected; the first access to a row reads
 * back a small band of rows, write faults mark rows dirty, and only dirty rows
 * are uploaded again. Render targets use a pitch that is a multiple of the
 * page size so that every row maps to whole pages. */
#define ROW_UNLOADED 0
#define ROW_CLEAN 1
#define ROW_WRITTEN 2
#define ROW_OVL 3
static struct { ddsurf_t *s; uint8_t *base; size_t len; uint8_t *rows; int h; int overlay; } g_guard;
static void flush_dirty(ddsurf_t *s);

static void mark_row_dirty(sx_t *x, int r) {
    if (!x->dirty) return;
    if (!x->dirty[r]) { x->dirty[r] = 1; x->ndirty++; }
    x->rowok[r] = 1;
    x->gpu_valid = 0;
}
static void ensure_dirty_map(ddsurf_t *s, sx_t *x) {
    if (!x->dirty || x->dirty_h != s->h) {
        free(x->dirty); free(x->rowok); free(x->ovl);
        x->dirty = calloc(s->h, 1); x->rowok = calloc(s->h, 1); x->ovl = calloc(s->h, 1); x->dirty_h = s->h; x->ndirty = 0; x->novl = 0;
    }
}

/* read rows [y0, y0+n) of the target into the surface memory */
static uint32_t *g_xfer; static size_t g_xfer_n;    /* reusable conversion buffer */
static uint32_t *xfer_buf(size_t px) {
    if (px > g_xfer_n) { free(g_xfer); g_xfer = malloc(px * 4); g_xfer_n = px; }
    return g_xfer;
}

static void load_rows(ddsurf_t *s, int y0, int n) {
    sx_t *x = s->gpu;
    uint64_t t0 = rt_now_us();
    uint32_t *tmp = xfer_buf((size_t)s->w * s->h);
    RDR->target_download(x->tgt, tmp, s->w, y0, n);
    /* convert just these rows */
    ddsurf_t view = *s;
    view.h = n; view.bits = s->bits + (size_t)y0 * s->pitch;
    argb_to_surf(&view, tmp + (size_t)y0 * s->w);
    g_rows_loaded += n;
    g_t_read += rt_now_us() - t0;
    { extern void *g_dd_caller;
      int i; for (i = 0; i < 16 && g_rb_site[i] && g_rb_site[i] != g_dd_caller; i++) {}
      if (i < 16) { g_rb_site[i] = g_dd_caller; g_rb_rows[i] += n; } }
    ensure_dirty_map(s, x);
    memset(x->rowok + y0, 1, n);
}

static int guard_ok(ddsurf_t *s) {
    return s->own_bits && (s->pitch % 4096) == 0 && ((uintptr_t)s->bits % 4096) == 0;
}
/* ---- overlay locks ------------------------------------------------------
 * GP3's car/sprite span renderer (Lock from 0x5148da) only writes the back
 * buffer, except for shadow texels which read the pixel below (handled by
 * d3d_shadow_read). Instead of reading the GPU image back, rows touched in
 * such a lock are filled with a key colour; the new pixels are later drawn
 * over the GPU image (keyed), which also keeps the high-resolution
 * background. */
int g_overlay_locks = 1;
int g_nofault;               /* no page-fault / int3 tricks (debugger attached) */
static uint8_t *g_nf_rows; static int g_nf_rows_h;   /* exception-free locks: rows to (re)check at the next lock */
/* lock sites verified (access tracing) to only write the back buffer, apart from the
 * shadow texels: the car/sprite span renderer and the message text renderer */
#include "gamepatch.h"
static int overlay_site(void *caller) {
    const gp3_addrs_t *A = gp3_addrs();
    if (!A) return 0;
    uint32_t c = (uint32_t)(uintptr_t)caller;
    return c == A->ovl_sites[0] || c == A->ovl_sites[1];
}
static uint16_t ovl_key(ddsurf_t *s) { return s->pf.gmask == 0x3e0 ? 0x7c1f : 0xf81f; }
/* horizontal extent of non-key pixels in a row; returns 0 if the row holds only key */
static int row_extent(const uint16_t *p, int w, uint16_t k, int *x0, int *x1) {
    int a = 0, b = w;
    while (a < w && p[a] == k) a++;
    if (a == w) return 0;
    while (b > a && p[b - 1] == k) b--;
    *x0 = a; *x1 = b;
    return 1;
}
static void flush_ovl(ddsurf_t *s) {
    sx_t *x = s->gpu;
    if (!x || !x->ovl || !x->novl || !x->tgt) return;
    uint64_t t0 = rt_now_us(); g_ovl_flushes++;
    uint16_t k = ovl_key(s);
    /* 1. find the runs of overlay rows that contain pixels, with their extents */
    static struct { int x0, y0, x1, y1; } runs[4096]; int nr = 0; long area = 0;
    int bx0 = s->w, by0 = s->h, bx1 = 0, by1 = 0;
    int r = 0;
    while (r < s->h && nr < 4096) {
        int x0, x1;
        if (!x->ovl[r] || !row_extent((const uint16_t *)(s->bits + (size_t)r * s->pitch), s->w, k, &x0, &x1)) { r++; continue; }
        int y0 = r, y1 = r + 1;
        while (y1 < s->h && x->ovl[y1] && y1 - y0 < 64) {
            int a, b;
            if (!row_extent((const uint16_t *)(s->bits + (size_t)y1 * s->pitch), s->w, k, &a, &b)) break;
            int nx0 = a < x0 ? a : x0, nx1 = b > x1 ? b : x1;
            if ((nx1 - nx0) > 2 * (x1 - x0) + 64) break;
            x0 = nx0; x1 = nx1; y1++;
        }
        runs[nr].x0 = x0; runs[nr].y0 = y0; runs[nr].x1 = x1; runs[nr].y1 = y1; nr++;
        area += (long)(x1 - x0) * (y1 - y0);
        if (x0 < bx0) bx0 = x0; if (y0 < by0) by0 = y0; if (x1 > bx1) bx1 = x1; if (y1 > by1) by1 = y1;
        r = y1;
    }
    long barea = (long)(bx1 - bx0) * (by1 - by0);
    if (nr > 2 && barea <= 2 * area + 16384) {
        /* 2a. many runs: one upload of their bounding box (transparent between runs).
         * Every separate upload of CPU pixels can make the driver wait for the GPU. */
        int bw = bx1 - bx0, bh = by1 - by0;
        uint32_t *box = xfer_buf((size_t)bw * bh * 2);
        uint32_t *tmp = box + (size_t)bw * bh;
        memset(box, 0, (size_t)bw * bh * 4);
        for (int i = 0; i < nr; i++) {
            int w = runs[i].x1 - runs[i].x0, h = runs[i].y1 - runs[i].y0;
            ddsurf_t view = *s; view.w = w; view.h = h; view.bits = s->bits + (size_t)runs[i].y0 * s->pitch + (size_t)runs[i].x0 * 2;
            view.ck_src.lo = view.ck_src.hi = k;
            surf_to_argb(&view, tmp, 1);                 /* key -> alpha 0 */
            for (int y = 0; y < h; y++) memcpy(box + (size_t)(runs[i].y0 - by0 + y) * bw + (runs[i].x0 - bx0), tmp + (size_t)y * w, (size_t)w * 4);
            g_ovl_rows += h; g_ovl_px += (uint64_t)w * h;
        }
        { uint64_t tg = rt_now_us(); RDR->blit(x->tgt, bx0, by0, bw, bh, box, bw, bh, bw, 1); g_t_ovl_gl += rt_now_us() - tg; }
    } else {
        /* 2b. few runs (or far apart): one upload each */
        uint32_t *tmp = xfer_buf((size_t)s->w * s->h);
        for (int i = 0; i < nr; i++) {
            int w = runs[i].x1 - runs[i].x0, h = runs[i].y1 - runs[i].y0;
            ddsurf_t view = *s; view.w = w; view.h = h; view.bits = s->bits + (size_t)runs[i].y0 * s->pitch + (size_t)runs[i].x0 * 2;
            view.ck_src.lo = view.ck_src.hi = k;
            surf_to_argb(&view, tmp, 1);
            { uint64_t tg = rt_now_us(); RDR->blit(x->tgt, runs[i].x0, runs[i].y0, w, h, tmp, w, h, w, 1); g_t_ovl_gl += rt_now_us() - tg; }
            g_ovl_rows += h; g_ovl_px += (uint64_t)w * h;
        }
    }
    if (g_nf_rows && g_nf_rows_h == s->h) for (int i = 0; i < s->h; i++) if (x->ovl[i]) g_nf_rows[i] = 1;
    memset(x->ovl, 0, s->h);
    x->novl = 0;
    g_t_upload += rt_now_us() - t0;
}
static void fill_key_row(ddsurf_t *s, int r) {
    uint16_t k = ovl_key(s), *p = (uint16_t *)(s->bits + (size_t)r * s->pitch);
    for (int i = 0; i < s->w; i++) p[i] = k;
}
int d3d_guard_fault(void *addr, int is_write);

/* ---- shadow texels in overlay locks -------------------------------------
 * The 16-bit span routines draw texel 1 as a shadow: `mov cl, byte ptr [edi]`
 * reads the pixel below. Those 76 instructions are replaced by int3; the trap
 * handler supplies the real byte (overlay pixel written in this lock, or the
 * GPU image read back for that one row). */
static const uint32_t g_shadow_sites[] = {
    0x4d5a5c, 0x4d5b37, 0x4d5bf8, 0x4d5cb1, 0x4d5d9b, 0x4d5e54, 0x4d5f6f, 0x4d6028,
    0x4d60e9, 0x4d61a2, 0x4d6263, 0x4d631c, 0x4d63dd, 0x4d6496, 0x4d6557, 0x4d6610,
    0x4d66d1, 0x4d678a, 0x4d684b, 0x4d6904, 0x4d69c5, 0x4d6a7e, 0x4d6b3f, 0x4d6bf8,
    0x4d6cb9, 0x4d6d72, 0x4d6e33, 0x4d6eec, 0x4d6fad, 0x4d7066, 0x4d7127, 0x4d71e0,
    0x4d72a1, 0x4d735a, 0x4d741b, 0x4d74d4, 0x4d7595, 0x4d764e, 0x4d7747, 0x4d784c,
    0x4d792f, 0x4d7a12, 0x4d7b18, 0x4d7bfb, 0x4d7d3f, 0x4d7e22, 0x4d7f05, 0x4d7fe8,
    0x4d80cb, 0x4d81ae, 0x4d8291, 0x4d8374, 0x4d8457, 0x4d853a, 0x4d861d, 0x4d8700,
    0x4d87e3, 0x4d88c6, 0x4d89a9, 0x4d8a8c, 0x4d8b6f, 0x4d8c52, 0x4d8d35, 0x4d8e18,
    0x4d8efb, 0x4d8fde, 0x4d90c1, 0x4d91a4, 0x4d9287, 0x4d936a, 0x4d944d, 0x4d9530,
    0x4d9613, 0x4d96f6, 0x4d97d9, 0x4d98bc
};
static uint16_t *g_real; static uint8_t *g_real_ok; static int g_real_w, g_real_h;
uint32_t g_extra_shadow_site;   /* self-test */
int d3d_is_shadow_site(uint32_t a) {
    if (g_extra_shadow_site && a == g_extra_shadow_site) return 1;
    if (!g_overlay_locks) return 0;
    const gp3_addrs_t *A = gp3_addrs();
    if (!A) return 0;
    a -= (uint32_t)A->shadow_delta;
    for (unsigned i = 0; i < sizeof g_shadow_sites / sizeof g_shadow_sites[0]; i++) if (g_shadow_sites[i] == a) return 1;
    return 0;
}
static void real_reset(ddsurf_t *s) {
    if (!g_real || g_real_w != s->w || g_real_h != s->h) {
        free(g_real); free(g_real_ok);
        g_real = malloc((size_t)s->w * s->h * 2); g_real_ok = calloc(s->h, 1); g_real_w = s->w; g_real_h = s->h;
    } else memset(g_real_ok, 0, s->h);
}
/* value of the byte at addr as the game would read it */
static ddsurf_t *g_nf_surf;
int d3d_shadow_read(uint32_t addr, uint8_t *out) {
    uint8_t *a = (uint8_t *)(uintptr_t)addr;
    g_shadow_traps++;
    if (g_nf_surf) {
        /* exception-free overlay lock: the buffer holds the key colour where nothing was drawn */
        ddsurf_t *s = g_nf_surf; sx_t *x = s->gpu;
        if (a >= s->bits && a < s->bits + (size_t)s->pitch * s->h) {
            size_t off = (size_t)(a - s->bits);
            int r = (int)(off / s->pitch), px = (int)((off % s->pitch) / 2);
            if (px < s->w) {
                uint16_t v = *(uint16_t *)(s->bits + (size_t)r * s->pitch + (size_t)px * 2);
                if (v != ovl_key(s)) { *out = *a; return 1; }
                if (!g_real_ok[r]) {
                    uint32_t *tmp = xfer_buf((size_t)s->w * s->h);
                    RDR->target_download(x->tgt, tmp, s->w, r, 1);
                    ddsurf_t view = *s; view.h = 1; view.pitch = s->w * 2; view.bits = (uint8_t *)(g_real + (size_t)r * s->w);
                    argb_to_surf(&view, tmp + (size_t)r * s->w);
                    g_real_ok[r] = 1; g_rows_loaded++;
                }
                *out = ((uint8_t *)(g_real + (size_t)r * s->w + px))[off & 1];
                return 1;
            }
        }
    }
    if (g_guard.s && a >= g_guard.base && a < g_guard.base + g_guard.len) {
        ddsurf_t *s = g_guard.s; sx_t *x = s->gpu;
        size_t off = (size_t)(a - g_guard.base);
        int r = (int)(off / s->pitch), px = (int)((off % s->pitch) / 2);
        int st = g_guard.rows[r];
        if (g_guard.overlay && (st == ROW_OVL || st == ROW_UNLOADED) && px < s->w) {
            if (st == ROW_OVL) {
                uint16_t v = *(uint16_t *)(g_guard.base + (size_t)r * s->pitch + (size_t)px * 2);
                if (v != ovl_key(s)) { *out = *a; return 1; }
            }
            if (!g_real_ok[r]) {
                uint32_t *tmp = xfer_buf((size_t)s->w * s->h);
                RDR->target_download(x->tgt, tmp, s->w, r, 1);
                ddsurf_t view = *s; view.h = 1; view.pitch = s->w * 2; view.bits = (uint8_t *)(g_real + (size_t)r * s->w);
                argb_to_surf(&view, tmp + (size_t)r * s->w);
                g_real_ok[r] = 1; g_rows_loaded++;
            }
            *out = ((uint8_t *)(g_real + (size_t)r * s->w + px))[off & 1];
            return 1;
        }
        if (st == ROW_UNLOADED) d3d_guard_fault(a, 0);      /* normal guarded lock: load the row */
    }
    *out = *a;
    return 1;
}
extern int plat_patch_code(void *addr, const void *bytes, int n);
__attribute__((cdecl, used, noinline)) static uint32_t shadow_byte_c(uint32_t edi) { uint8_t v = 0; d3d_shadow_read(edi, &v); return v; }
/* code space for the shadow stub (filled by d3d_install_shadow_traps) */
__attribute__((naked, used, noinline)) static void shadow_stub_area(void) {
    __asm__ volatile(".fill 32, 1, 0xcc");
}
/* stub: pushad; push edi; call shadow_byte_c; add esp,4; mov [esp+24],al; popad; <add eax,[abs]>; ret */
uint8_t *d3d_build_shadow_stub(const uint8_t *addinsn) {
    uint8_t code[24]; int k = 0;
    uint8_t *stub = (uint8_t *)(uintptr_t)shadow_stub_area;
    code[k++] = 0x60; code[k++] = 0x57;
    code[k++] = 0xe8; int32_t rel = (int32_t)((uintptr_t)shadow_byte_c - ((uintptr_t)stub + k + 4)); memcpy(code + k, &rel, 4); k += 4;
    code[k++] = 0x83; code[k++] = 0xc4; code[k++] = 0x04;
    code[k++] = 0x88; code[k++] = 0x44; code[k++] = 0x24; code[k++] = 0x18;
    code[k++] = 0x61;
    memcpy(code + k, addinsn, 6); k += 6;
    code[k++] = 0xc3;
    return plat_patch_code(stub, code, k) ? NULL : stub;
}
void d3d_install_shadow_traps(void) {
    static int done;
    if (done || !g_overlay_locks) return;
    done = 1;
    const gp3_addrs_t *A = gp3_addrs();
    if (!A) { LOG("overlay locks disabled: unknown Gp3.exe build"); g_overlay_locks = 0; return; }
    unsigned n = sizeof g_shadow_sites / sizeof g_shadow_sites[0];
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)(g_shadow_sites[i] + (uint32_t)A->shadow_delta);
        if (p[0] != 0x8a || p[1] != 0x0f) {
            LOG("overlay locks disabled: unexpected code at %08x (%s)", g_shadow_sites[i] + (uint32_t)A->shadow_delta, A->name);
            g_overlay_locks = 0; return;
        }
    }
    if (g_nofault) {
        /* A debugger (GPxPatch) receives every int3 first: instead, the 8 bytes
         * `mov cl,[edi]; add eax,[abs]` (identical at all sites) become a call to a
         * stub that fetches the real byte and then performs the add (its flags are
         * used by the next instruction; call/ret do not change flags). */
        const uint8_t *p0 = (const uint8_t *)(uintptr_t)(g_shadow_sites[0] + (uint32_t)A->shadow_delta);
        for (unsigned i = 0; i < n; i++) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)(g_shadow_sites[i] + (uint32_t)A->shadow_delta);
            if (memcmp(p, p0, 8) || p[2] != 0x03 || p[3] != 0x05) { LOG("overlay locks: shadow sites differ, shadows not handled"); return; }
        }
        uint8_t *stub = d3d_build_shadow_stub(p0 + 2);
        if (!stub) { LOG("overlay locks: could not write the shadow stub"); return; }
        for (unsigned i = 0; i < n; i++) {
            uint8_t *site = (uint8_t *)(uintptr_t)(g_shadow_sites[i] + (uint32_t)A->shadow_delta);
            uint8_t c[8] = { 0xe8, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
            int32_t r2 = (int32_t)((uintptr_t)stub - ((uintptr_t)site + 5)); memcpy(c + 1, &r2, 4);
            plat_patch_code(site, c, 8);
        }
        LOG("overlay locks enabled (%u shadow sites patched, debugger attached)", n);
        return;
    }
    static const uint8_t trap[2] = { 0xcc, 0x90 };
    for (unsigned i = 0; i < n; i++) plat_patch_code((void *)(uintptr_t)(g_shadow_sites[i] + (uint32_t)A->shadow_delta), trap, 2);
    LOG("overlay locks enabled (%u shadow sites trapped)", n);
}

static void guard_arm(ddsurf_t *s) {
    static int first = 1;
    if (first) { first = 0; LOG("lazy readback: first guarded lock (%dx%d)", s->w, s->h); }
    sx_t *x = s->gpu;
    extern void *g_dd_caller;
    int overlay = g_overlay_locks && s->bpp == 16 && overlay_site(g_dd_caller);
    if (!overlay) flush_ovl(s);
    g_guard.s = s; g_guard.base = s->bits; g_guard.h = s->h; g_guard.overlay = overlay;
    g_guard.len = (size_t)s->pitch * s->h;
    g_guard.rows = calloc(s->h, 1);
    plat_protect(g_guard.base, g_guard.len, 0);
    if (overlay) real_reset(s);
    if (overlay && x->ovl) for (int r = 0; r < s->h; r++) if (x->ovl[r]) {
        g_guard.rows[r] = ROW_OVL;
        plat_protect(g_guard.base + (size_t)r * s->pitch, s->pitch, 2);
    }
    if (x->rowok) for (int r = 0; r < s->h; r++) if (x->rowok[r]) {
        /* already current in memory: readable; dirty rows writable */
        g_guard.rows[r] = x->dirty[r] ? ROW_WRITTEN : ROW_CLEAN;
        plat_protect(g_guard.base + (size_t)r * s->pitch, s->pitch, x->dirty[r] ? 2 : 1);
    }
}
uint32_t g_fault_eip;
static uint32_t g_site_eip[256], g_site_n[256][2]; static int g_nsites, g_probe_sites = -1;
static void site_record(int w) {
    int i; for (i = 0; i < g_nsites && g_site_eip[i] != g_fault_eip; i++) {}
    if (i == g_nsites && g_nsites < 256) g_site_eip[g_nsites++] = g_fault_eip;
    if (i < 256) g_site_n[i][w]++;
    static int calls; if (++calls % 20000 == 0) {
        FILE *f = fopen("out/sites.txt", "w");
        if (f) { for (int k = 0; k < g_nsites; k++) fprintf(f, "%08x r %u w %u\n", g_site_eip[k], g_site_n[k][0], g_site_n[k][1]); fclose(f); }
    }
}
int d3d_guard_fault(void *addr, int is_write) {
    uint8_t *a = addr;
    if (!g_guard.s || a < g_guard.base || a >= g_guard.base + g_guard.len) return 0;
    ddsurf_t *s = g_guard.s; sx_t *x = s->gpu;
    int r = (int)((size_t)(a - g_guard.base) / s->pitch);
    if (g_probe_sites < 0) g_probe_sites = getenv("GP3_SITES") != NULL;
    if (g_probe_sites) {   /* diagnostics: one row per fault so every first touch reports its instruction */
        site_record(is_write);
        if (g_guard.rows[r] == ROW_UNLOADED) {
            plat_protect(g_guard.base + (size_t)r * s->pitch, s->pitch, 2);
            load_rows(s, r, 1);
            plat_protect(g_guard.base + (size_t)r * s->pitch, s->pitch, 1);
            g_guard.rows[r] = ROW_CLEAN;
            return 1;
        }
    }
    if (g_guard.rows[r] == ROW_UNLOADED && g_guard.overlay && is_write) {
        /* overlay lock: no read-back, the row starts as key colour */
        plat_protect(g_guard.base + (size_t)r * s->pitch, s->pitch, 2);
        fill_key_row(s, r);
        g_guard.rows[r] = ROW_OVL;
        if (!x->ovl[r]) { x->ovl[r] = 1; x->novl++; }
        x->rowok[r] = 0; x->gpu_valid = 0;
        return 1;
    }
    if (g_guard.rows[r] == ROW_UNLOADED) {
        /* load a band of consecutive unloaded rows around the access (span renderers walk row by row) */
        int y0 = r, y1 = r + 1;
        while (y0 > 0 && r - y0 < 16 && g_guard.rows[y0 - 1] == ROW_UNLOADED) y0--;
        while (y1 < s->h && y1 - r < 112 && g_guard.rows[y1] == ROW_UNLOADED) y1++;
        plat_protect(g_guard.base + (size_t)y0 * s->pitch, (size_t)(y1 - y0) * s->pitch, 2);
        load_rows(s, y0, y1 - y0);
        plat_protect(g_guard.base + (size_t)y0 * s->pitch, (size_t)(y1 - y0) * s->pitch, 1);
        for (int i = y0; i < y1; i++) g_guard.rows[i] = ROW_CLEAN;
        return 1;   /* retry; a write faults once more and is recorded below */
    }
    if (g_guard.rows[r] == ROW_CLEAN && is_write) {
        plat_protect(g_guard.base + (size_t)r * s->pitch, s->pitch, 2);
        g_guard.rows[r] = ROW_WRITTEN;
        mark_row_dirty(x, r);
        return 1;
    }
    /* unexpected: make the row accessible to avoid an endless fault loop */
    plat_protect(g_guard.base + (size_t)r * s->pitch, s->pitch, 2);
    return 1;
}
/* end the guard; if full is set, the whole CPU image is made current */
static void guard_release(ddsurf_t *s, int full) {
    if (g_guard.s != s) return;
    sx_t *x = s->gpu;
    plat_protect(g_guard.base, g_guard.len, 2);
    int all = 1;
    if (x->novl) {
        if (full) {      /* the CPU image must become real: composite the overlay and read those rows back */
            for (int r = 0; r < s->h; r++) if (x->ovl[r]) g_guard.rows[r] = ROW_UNLOADED;
            flush_ovl(s);
        } else all = 0;
    }
    for (int r = 0; r < s->h; r++) {
        if (g_guard.rows[r] != ROW_UNLOADED) continue;
        if (!full) { all = 0; continue; }
        int y1 = r; while (y1 < s->h && g_guard.rows[y1] == ROW_UNLOADED) y1++;
        load_rows(s, r, y1 - r);
        for (int i = r; i < y1; i++) g_guard.rows[i] = ROW_CLEAN;
        r = y1 - 1;
    }
    x->cpu_valid = all;
    free(g_guard.rows); g_guard.rows = NULL;
    g_guard.s = NULL;
}

static void download_now(ddsurf_t *s) {
    sx_t *x = s->gpu;
    if (!x || !x->tgt || x->cpu_valid) return;
    g_st.downloads++;
    ensure_dirty_map(s, x);
    flush_ovl(s);
    for (int r = 0; r < s->h; r++) {      /* rows already current in memory (incl. dirty ones) are kept */
        if (x->rowok[r]) continue;
        int y1 = r; while (y1 < s->h && !x->rowok[y1]) y1++;
        load_rows(s, r, y1 - r);
        r = y1 - 1;
    }
    x->cpu_valid = 1;
}
/* make sure the CPU copy is current (before our own code reads/writes it) */
static void cpu_need(ddsurf_t *s) {
    if (!s || !s->gpu) return;
    if (g_guard.s == s) { guard_release(s, 1); return; }
    sx_t *x = s->gpu;
    if (x->cpu_valid || !x->tgt) return;
    if (g_trace_d3d) { extern void *g_dd_caller; DTRACE("rt download %p %dx%d (game caller %p)", (void *)s, s->w, s->h, g_dd_caller); }
    download_now(s);
}
/* the whole CPU image was (potentially) modified */
static void cpu_written(ddsurf_t *s) {
    if (!s || !s->gpu) return;
    sx_t *x = s->gpu;
    if (g_guard.s == s) guard_release(s, 1);
    if (!x->tgt) { x->cpu_valid = 1; return; }
    ensure_dirty_map(s, x);
    memset(x->dirty, 1, s->h); x->ndirty = s->h;
    memset(x->ovl, 0, s->h); x->novl = 0;     /* the full CPU image supersedes pending overlays */
    x->cpu_valid = 1; x->gpu_valid = 0;
}
/* upload dirty rows (in runs) */
static void flush_dirty(ddsurf_t *s) {
    sx_t *x = s->gpu;
    if (x->tgt && x->novl && (!x->dirty || !x->ndirty)) { flush_ovl(s); x->gpu_valid = 1; return; }
    if (!x->tgt || !x->dirty || !x->ndirty) { if (x->tgt && !x->dirty) x->gpu_valid = 1; return; }
    uint64_t t0 = rt_now_us();
    uint32_t *tmp = xfer_buf((size_t)s->w * s->h);
    g_st.uploads++;
    for (int r = 0; r < s->h; r++) {
        if (!x->dirty[r]) continue;
        int y1 = r; while (y1 < s->h && x->dirty[y1]) y1++;
        ddsurf_t view = *s; view.h = y1 - r; view.bits = s->bits + (size_t)r * s->pitch;
        surf_to_argb(&view, tmp + (size_t)r * s->w, 0);
        RDR->target_upload(x->tgt, tmp, s->w, 0, r, s->w, y1 - r);
        g_rows_uploaded += y1 - r;
        memset(x->dirty + r, 0, y1 - r);
        r = y1 - 1;
    }
    x->ndirty = 0;
    g_t_upload += rt_now_us() - t0;
    flush_ovl(s);
    x->gpu_valid = 1;
}
/* make sure the backend render target exists and holds current data */
static rtarget_t *gpu_need(ddsurf_t *s) {
    sx_t *x = sx_of(s);
    if (g_guard.s == s) guard_release(s, 0);
    if (!x->tgt) {
        x->tgt = RDR->target_create(s->w, s->h, g_render_scale);
        ensure_dirty_map(s, x);
        memset(x->dirty, 1, s->h); x->ndirty = s->h;   /* initial contents come from the CPU */
        x->gpu_valid = 0;
    }
    ensure_dirty_map(s, x);
    if (!x->gpu_valid) flush_dirty(s);
    return x->tgt;
}
static void gpu_written(ddsurf_t *s) {
    sx_t *x = sx_of(s); x->gpu_valid = 1; x->cpu_valid = 0;
    if (x->rowok) memset(x->rowok, 0, s->h);
}

/* hooks called from ddraw.c */
int g_lazy_readback = 1;
/* Exception-free mode (a debugger such as GPxPatch is attached: every page fault
 * or int3 would be routed through it): overlay locks fill the whole buffer with
 * the key colour and find the written rows at Unlock instead of page faults.
 * Rows are compared with a key row first (fast) so only changed rows are touched. */
static uint16_t *g_keyrow; static int g_keyrow_w; static uint16_t g_keyrow_k;
extern int plat_write_watch(void *base, size_t len, int reset, void **pages, size_t *n);
static ddsurf_t *g_nf_init;              /* surface whose non-overlay rows are known to hold the key colour */
static void **g_ww_pages; static size_t g_ww_cap;
/* mark the rows written since the last reset (write watch); returns 0 if not available */
static int ww_rows(ddsurf_t *s, int reset, uint8_t *rows) {
    size_t len = (size_t)s->pitch * s->h, cap = len / 4096 + 2;
    if (g_ww_cap < cap) { free(g_ww_pages); g_ww_pages = malloc(cap * sizeof *g_ww_pages); g_ww_cap = cap; }
    size_t n = cap;
    if (plat_write_watch(s->bits, len, reset, g_ww_pages, &n)) return 0;
    for (size_t i = 0; i < n; i++) {
        size_t a = (size_t)((uint8_t *)g_ww_pages[i] - s->bits), b = a + 4096;
        int r0 = (int)(a / s->pitch), r1 = (int)((b + s->pitch - 1) / s->pitch);
        if (r1 > s->h) r1 = s->h;
        for (int r = r0; r < r1; r++) rows[r] = 1;
    }
    return 1;
}
static const uint16_t *key_row(ddsurf_t *s) {
    uint16_t k = ovl_key(s);
    if (!g_keyrow || g_keyrow_w < s->w || g_keyrow_k != k) {
        free(g_keyrow); g_keyrow = malloc((size_t)s->w * 2); g_keyrow_w = s->w; g_keyrow_k = k;
        for (int i = 0; i < s->w; i++) g_keyrow[i] = k;
    }
    return g_keyrow;
}
static void d3d_lock_hook2(ddsurf_t *s, DWORD flags);
void d3d_lock_hook(ddsurf_t *s, DWORD flags) { uint64_t t0 = rt_now_us(); d3d_lock_hook2(s, flags); g_t_lock += rt_now_us() - t0; }
static void d3d_unlock_hook2(ddsurf_t *s, int readonly);
void d3d_unlock_hook(ddsurf_t *s, int readonly) { uint64_t t0 = rt_now_us(); d3d_unlock_hook2(s, readonly); g_t_lock += rt_now_us() - t0; }
static void d3d_lock_hook2(ddsurf_t *s, DWORD flags) {
    (void)flags;
    sx_t *x = s->gpu;
    extern void *g_dd_caller;
    if (g_nofault && g_overlay_locks && x && x->tgt && !x->cpu_valid && s->bpp == 16 && !g_guard.s && !g_nf_surf &&
        overlay_site(g_dd_caller)) {
        ensure_dirty_map(s, x);
        if (x->ndirty) flush_dirty(s);                 /* pending full rows go to the GPU first */
        const uint16_t *kr = key_row(s); size_t rb = (size_t)s->w * 2;
        if (!g_nf_rows || g_nf_rows_h != s->h) { free(g_nf_rows); g_nf_rows = calloc(s->h, 1); g_nf_rows_h = s->h; g_nf_init = NULL; }
        int all = g_nf_init != s || !ww_rows(s, 0, g_nf_rows);   /* rows written since the last lock (by anyone) */
        for (int r = 0; r < s->h; r++) if ((all || g_nf_rows[r]) && !x->ovl[r]) {   /* pending overlay rows keep their pixels */
            uint8_t *row = s->bits + (size_t)r * s->pitch;
            if (memcmp(row, kr, rb)) memcpy(row, kr, rb);
        }
        memset(g_nf_rows, 0, s->h);
        if (ww_rows(s, 1, g_nf_rows)) { memset(g_nf_rows, 0, s->h); g_nf_init = s; }   /* reset: from here on, game writes */
        else g_nf_init = NULL;
        memset(x->rowok, 0, s->h);
        real_reset(s);
        g_nf_surf = s;
        return;
    }
    if (g_lazy_readback && x && x->tgt && !x->cpu_valid && !g_guard.s && guard_ok(s)) {
        guard_arm(s);
        return;
    }
    cpu_need(s);
}
static void d3d_unlock_hook2(ddsurf_t *s, int readonly) {
    if (g_nf_surf == s) {
        g_nf_surf = NULL;
        sx_t *x = s->gpu; uint16_t k = ovl_key(s);
        const uint16_t *kr = key_row(s); size_t rb = (size_t)s->w * 2; (void)k;
        int all = g_nf_init != s || !ww_rows(s, 0, g_nf_rows);
        for (int r = 0; r < s->h; r++) {
            if (x->ovl[r] || (!all && !g_nf_rows[r])) continue;
            if (memcmp(s->bits + (size_t)r * s->pitch, kr, rb)) { x->ovl[r] = 1; x->novl++; }
        }
        /* rows found here stay marked in g_nf_rows: after they are flushed they must be re-keyed */
        if (x->novl) x->gpu_valid = 0;
        x->cpu_valid = 0;
        return;
    }
    if (g_guard.s == s) {
        int touched = 0;
        for (int r = 0; r < s->h && !touched; r++) touched = g_guard.rows[r] != ROW_UNLOADED;
        guard_release(s, 0);           /* written rows were recorded by the fault handler */
        if (!touched) g_guard_skipped++;
        return;
    }
    if (!readonly) cpu_written(s);
}
int d3d_blit_hook(ddsurf_t *d, RECT dr, ddsurf_t *s, RECT sr, DWORD flags) {
    (void)dr; (void)sr; (void)flags;
    cpu_need(s); cpu_need(d); cpu_written(d);
    return 0;
}
void d3d_cpu_need(ddsurf_t *s) { cpu_need(s); }
/* source rows needed by a CPU blit: read back just those rows (does not make the whole CPU copy valid) */
void d3d_cpu_need_rows(ddsurf_t *s, int y0, int y1) {
    sx_t *x = s ? s->gpu : NULL;
    if (!x || !x->tgt || x->cpu_valid || g_guard.s == s) { cpu_need(s); return; }
    if (y0 < 0) y0 = 0;
    if (y1 > s->h) y1 = s->h;
    if (y1 <= y0) return;
    ensure_dirty_map(s, x);
    flush_ovl(s);
    for (int r = y0; r < y1; r++) {
        if (x->rowok[r]) continue;
        int e = r; while (e < y1 && !x->rowok[e]) e++;
        load_rows(s, r, e - r);
        r = e - 1;
    }
}
/* rows [y0,y1) were modified in CPU memory */
void d3d_cpu_written_rows(ddsurf_t *s, int y0, int y1) {
    sx_t *x = s ? s->gpu : NULL;
    if (!x) return;
    if (g_guard.s == s) guard_release(s, 1);
    if (!x->tgt) { return; }
    if (y0 < 0) y0 = 0;
    if (y1 > s->h) y1 = s->h;
    ensure_dirty_map(s, x);
    for (int r = y0; r < y1; r++) mark_row_dirty(x, r);
}
/* DirectDraw blits whose destination is a 3D render target are executed by the
 * renderer (colour fills -> clears, copies from CPU surfaces -> textured quads). */
/* source image lives on the GPU (CPU copy not current) */
static int gpu_resident(ddsurf_t *s) { sx_t *x = s ? s->gpu : NULL; return x && x->tgt && !x->cpu_valid; }
int d3d_gpu_blit(ddsurf_t *d, const RECT *dr_, ddsurf_t *s, const RECT *sr_, DWORD flags, const void *fx_) {
    /* plain copy from a GPU-resident image into an offscreen plain surface (GP3 saves the
     * cockpit strip this way every frame): give the destination a GPU image too */
    if (g_rdr && s && s != d && gpu_resident(s) && !(d->gpu && ((sx_t *)d->gpu)->tgt) && !(flags & ~0x1000000u) &&
        !(d->caps.caps & (0x1000 | 0x200 | 0x20000)) && d->bpp == s->bpp && d->bpp >= 15 && !d->is_primary) {
        LOG("surface %p %dx%d now GPU resident (copied from a 3D surface)", (void *)d, d->w, d->h);
        gpu_need(d);
    }
    sx_t *x = d->gpu;
    if (!x || !x->tgt || !g_rdr) return 0;
    if (x->cpu_valid && !(s && s != d && gpu_resident(s))) return 0;   /* CPU copy is current: plain CPU blit */
    if (flags & (0x2000 | 0x4000 | 0x2000000)) return 0;     /* dest colour keys / depth fill: CPU path */
    const uint32_t *fx = fx_;
    if ((flags & 0x800) && fx && (fx[1] & 6)) return 0;      /* mirroring: CPU path */
    RECT dr = dr_ ? *dr_ : (RECT){ 0, 0, d->w, d->h };
    if (dr.left < 0) dr.left = 0; if (dr.top < 0) dr.top = 0;
    if (dr.right > d->w) dr.right = d->w; if (dr.bottom > d->h) dr.bottom = d->h;
    if (dr.right <= dr.left || dr.bottom <= dr.top) return 1;
    rtarget_t *t = gpu_need(d);
    if (flags & 0x400) {   /* DDBLT_COLORFILL: fx->dwFillColor is at offset 80 */
        uint32_t v = fx ? fx[20] : 0;
        uint32_t argb;
        if (d->bpp == 8) argb = d->pal ? d->pal->xrgb[v & 255] | 0xff000000u : 0xff000000u;
        else argb = 0xff000000u | chan8(v, d->pf.rmask) << 16 | chan8(v, d->pf.gmask) << 8 | chan8(v, d->pf.bmask);
        int rect[4] = { dr.left, dr.top, dr.right, dr.bottom };
        RDR->clear(t, rect, 1, 1, argb, 0, 0, 0, 0, d->w, d->h);
        gpu_written(d);
        return 1;
    }
    if (!s) return 0;
    sx_t *sxs = s->gpu;
    RECT sr = sr_ ? *sr_ : (RECT){ 0, 0, s->w, s->h };
    if (sr.left < 0 || sr.top < 0 || sr.right > s->w || sr.bottom > s->h || sr.right <= sr.left || sr.bottom <= sr.top) return 0;
    if (s == d) {
        if (flags & (0x8000 | 0x10000)) return 0;            /* keyed self copy: CPU path */
        RDR->copy(t, dr.left, dr.top, dr.right - dr.left, dr.bottom - dr.top, sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top);
        gpu_written(d);
        return 1;
    }
    if (gpu_resident(s) && !(flags & (0x8000 | 0x10000))) {
        rtarget_t *st_ = gpu_need(s);
        RDR->copy_from(t, dr.left, dr.top, dr.right - dr.left, dr.bottom - dr.top, st_, sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top);
        gpu_written(d);
        g_gpu_copies++;
        return 1;
    }
    if (sxs && sxs->tgt && !sxs->cpu_valid) d3d_cpu_need_rows(s, sr.top, sr.bottom);
    int keyed = (flags & 0x8000) && s->has_ck_src;              /* DDBLT_KEYSRC */
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    uint32_t *tmp = malloc((size_t)sw * sh * 4);
    ddsurf_t view = *s;
    view.w = sw; view.h = sh;
    view.bits = s->bits + (size_t)sr.top * s->pitch + sr.left * ((s->bpp + 7) / 8);
    surf_to_argb(&view, tmp, keyed);
    if (flags & 0x10000) { /* KEYSRCOVERRIDE: fx->ddckSrcColorkey at offset 92 */
        ddsurf_t v2 = view; v2.ck_src.lo = fx[23]; v2.ck_src.hi = fx[24]; surf_to_argb(&v2, tmp, 1); keyed = 1;
    }
    RDR->blit(t, dr.left, dr.top, dr.right - dr.left, dr.bottom - dr.top, tmp, sw, sh, sw, keyed);
    free(tmp);
    gpu_written(d);
    return 1;
}
int d3d_is_gpu_target(ddsurf_t *s) { sx_t *x = s ? s->gpu : NULL; return x && x->tgt ? (x->gpu_valid ? 2 : 1) : 0; }
void d3d_cpu_written(ddsurf_t *s) { cpu_written(s); }
void d3d_flip_hook(ddsurf_t **chain, int n) { (void)chain; (void)n; /* sx_t pointers travel with the buffers */ }
int d3d_present_hook(ddsurf_t *front) {
    g_st.presents++; stats_tick();
    if (!front->gpu || !g_rdr) return 0;   /* no 3D yet: CPU presentation */
    sx_t *x = front->gpu;
    if (x->tgt && !x->cpu_valid && !x->gpu_valid && g_guard.s != front) flush_dirty(front);
    if (x->tgt && x->gpu_valid && !x->cpu_valid) {
        extern uint32_t g_diff_site;
        if (g_diff_site) {
            extern int ddraw_diff_overlay(uint32_t *buf, int w, int h);
            uint32_t *tmp = malloc((size_t)front->w * front->h * 4);
            RDR->target_download(x->tgt, tmp, front->w, 0, front->h);
            ddraw_diff_overlay(tmp, front->w, front->h);
            RDR->present_argb(tmp, front->w, front->h, front->w);
            free(tmp);
            return 1;
        }
        uint64_t tp = rt_now_us();
        RDR->present(x->tgt, front->w, front->h);
        g_t_present += rt_now_us() - tp;
        return 1;
    }
    return 0;
}
void d3d_surface_released(ddsurf_t *s) {
    sx_t *x = s->gpu;
    if (!x) return;
    if (x->tex && g_rdr) g_rdr->tex_destroy(x->tex);
    if (x->tgt && g_rdr) g_rdr->target_destroy(x->tgt);
    if (g_guard.s == s) { plat_protect(g_guard.base, g_guard.len, 2); free(g_guard.rows); g_guard.rows = NULL; g_guard.s = NULL; }
    free(x->dirty); free(x->rowok); free(x->ovl);
    free(x); s->gpu = NULL;
    if (g_render_target == s) g_render_target = NULL;
}
void d3d_depth_fill(ddsurf_t *z, RECT r, uint32_t v);

/* ------------------------------------------------------------------ */
/* textures                                                             */
static void dilate_keyed(uint32_t *p, int w, int h) {
    /* give transparent texels the colour of an opaque neighbour so that
     * bilinear filtering does not produce dark fringes */
    for (int pass = 0; pass < 2; pass++)
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint32_t *c = &p[y * w + x];
            if (*c >> 24) continue;
            int r = 0, g = 0, b = 0, n = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int X = x + dx, Y = y + dy; if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
                uint32_t q = p[Y * w + X];
                if (q >> 24) { r += (q >> 16) & 255; g += (q >> 8) & 255; b += q & 255; n++; }
            }
            if (n) *c = (uint32_t)(r / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(b / n);
        }
}
static uint32_t tex_version(ddsurf_t *s) {
    uint32_t v = s->version * 31u + (uint32_t)s->has_ck_src * 7u + s->ck_src.lo;
    if (s->pal) v += (uint32_t)s->pal->version * 131u;
    for (ddsurf_t *m = s->mip_next; m; m = m->mip_next) v = v * 31u + m->version;
    return v;
}
static rtex_t *tex_get(ddsurf_t *s) {
    if (!s) return NULL;
    sx_t *x = sx_of(s);
    if (x->tgt && x->gpu_valid && !x->cpu_valid) cpu_need(s);   /* rendered-to texture */
    uint32_t ver = tex_version(s);
    int levels = 1; for (ddsurf_t *m = s->mip_next; m; m = m->mip_next) levels++;
    if (x->tex && x->tex_ver == ver && x->tex_w == s->w && x->tex_h == s->h && x->tex_levels == levels) return x->tex;
    /* GP3 locks some textures every frame without changing them: compare contents first */
    uint64_t hsh = 0x9e3779b97f4a7c15ull ^ (s->has_ck_src ? s->ck_src.lo : 0xffffffffu);
    for (ddsurf_t *m = s; m; m = m->mip_next) {
        size_t rb = (size_t)m->w * ((m->bpp + 7) / 8);
        for (int y = 0; y < m->h; y++) {
            const uint8_t *r = m->bits + (size_t)y * m->pitch; size_t i = 0;
            for (; i + 8 <= rb; i += 8) { uint64_t v; memcpy(&v, r + i, 8); hsh = (hsh ^ v) * 0x100000001b3ull; hsh ^= hsh >> 29; }
            for (; i < rb; i++) hsh = (hsh ^ r[i]) * 0x100000001b3ull;
        }
    }
    if (x->tex && x->tex_w == s->w && x->tex_h == s->h && x->tex_levels == levels && x->tex_hash == hsh) {
        x->tex_ver = ver; g_tex_same++;
        return x->tex;
    }
    x->tex_hash = hsh;
    if (!x->tex || x->tex_w != s->w || x->tex_h != s->h || x->tex_levels != levels) {
        if (x->tex) RDR->tex_destroy(x->tex);
        x->tex = RDR->tex_create(s->w, s->h, levels);
        x->tex_w = s->w; x->tex_h = s->h; x->tex_levels = levels;
    }
    int lvl = 0;
    g_st.tex_uploads++; g_st.tex_bytes += s->w * s->h * 4;
    if (g_trace_d3d) { extern void *g_dd_caller; DTRACE("tex upload %p %dx%d ver %u (last dd caller %p)", (void *)s, s->w, s->h, ver, g_dd_caller); }
    uint64_t tt0 = rt_now_us();
    for (ddsurf_t *m = s; m; m = m->mip_next, lvl++) {
        uint32_t *tmp = xfer_buf((size_t)m->w * m->h);
        int ck = s->has_ck_src;
        if (ck) { m->ck_src = s->ck_src; }
        surf_to_argb(m, tmp, ck);
        if (ck) dilate_keyed(tmp, m->w, m->h);
        RDR->tex_upload(x->tex, lvl, m->w, m->h, tmp);
    }
    g_t_tex += rt_now_us() - tt0;
    x->tex_ver = ver;
    DTRACE("texture upload surf=%p rtex=%p %dx%d bpp=%d pf=%x masks=%x/%x/%x/%x ck=%d(%x) levels=%d",
           (void *)s, (void *)x->tex, s->w, s->h, s->bpp, s->pf.flags, s->pf.rmask, s->pf.gmask, s->pf.bmask, s->pf.amask, s->has_ck_src, s->ck_src.lo, levels);
    extern int g_dump_textures;
    if (g_dump_textures) {
        extern int write_png(const char *path, const uint32_t *xrgb, int w, int h, int pitch_px);
        uint32_t *tmp = malloc((size_t)s->w * s->h * 4);
        surf_to_argb(s, tmp, s->has_ck_src);
        for (int i = 0; i < s->w * s->h; i++) if (!(tmp[i] >> 24)) tmp[i] = 0x00ff00ff ^ (((i / s->w / 4 + i % s->w / 4) & 1) ? 0xffffff : 0); 
        char p[600]; snprintf(p, sizeof p, "%s/tex_%p.png", g_cfg.dump_dir, (void *)x->tex);
        write_png(p, tmp, s->w, s->h, s->w);
        free(tmp);
    }
    return x->tex;
}

/* ------------------------------------------------------------------ */
/* device                                                               */
#pragma pack(push, 1)
typedef struct { DWORD x, y, w, h; float minz, maxz; } D3DVIEWPORT7;
#pragma pack(pop)

/* D3D7 state block: only the states marked in the masks belong to the block */
typedef struct { DWORD rs[256]; DWORD tss[8][32]; ddsurf_t *tex[8]; int valid;
                 uint8_t rs_m[256], tss_m[8][32], tex_m[8]; } sblock_t;

typedef struct d3d7 { void **vtbl; LONG refs; ddraw_t *dd; } d3d7_t;
typedef struct d3ddev {
    void **vtbl; LONG refs;
    d3d7_t *d3d;
    ddsurf_t *rt;
    DWORD rs[256];
    DWORD tss[8][32];
    ddsurf_t *tex[8];
    D3DVIEWPORT7 vp;
    float mat[3][16];   /* world, view, projection */
    uint8_t material[68];
    uint8_t lights[16][104]; int light_on[16];
    uint8_t clipstatus[24];
    int in_scene;
    int recording; sblock_t *rec;
    int is_tnl;
    uint8_t guid[16];
} d3ddev_t;

static void **g_d3d7_vtbl, **g_dev_vtbl, **g_vb_vtbl;
static d3ddev_t *g_dev;
static sblock_t *g_sblocks[256];

static void dev_defaults(d3ddev_t *d) {
    memset(d->rs, 0, sizeof d->rs);
    memset(d->tss, 0, sizeof d->tss);
    d->rs[4] = 1;      /* TEXTUREPERSPECTIVE */
    d->rs[7] = d->rt && d->rt->zbuf ? 1 : 0;   /* ZENABLE */
    d->rs[8] = 3;      /* FILLMODE solid */
    d->rs[9] = 2;      /* SHADEMODE gouraud */
    d->rs[14] = 1;     /* ZWRITEENABLE */
    d->rs[16] = 1;     /* LASTPIXEL */
    d->rs[19] = 2;     /* SRCBLEND one */
    d->rs[20] = 1;     /* DESTBLEND zero */
    d->rs[22] = 3;     /* CULLMODE ccw */
    d->rs[23] = 4;     /* ZFUNC lessequal */
    d->rs[25] = 8;     /* ALPHAFUNC always */
    d->rs[37] = 0x3f800000; /* FOGEND 1.0 */
    d->rs[38] = 0x3f800000; /* FOGDENSITY 1.0 */
    d->rs[60] = 0xffffffff; /* TEXTUREFACTOR */
    d->rs[136] = 1;    /* CLIPPING */
    d->rs[137] = 1;    /* LIGHTING */
    d->rs[141] = 1;    /* COLORVERTEX */
    d->rs[145] = 1; d->rs[146] = 2;
    for (int i = 0; i < 8; i++) {
        DWORD *t = d->tss[i];
        t[1] = i == 0 ? 4 : 1;  /* COLOROP modulate / disable */
        t[2] = 2; t[3] = 1;     /* COLORARG1 texture, ARG2 current */
        t[4] = i == 0 ? 2 : 1;  /* ALPHAOP selectarg1 / disable */
        t[5] = 2; t[6] = 1;
        t[11] = i;              /* TEXCOORDINDEX */
        t[12] = t[13] = t[14] = 1;   /* ADDRESS wrap */
        t[16] = t[17] = 1; t[18] = 1;  /* point, no mip */
        t[21] = 1;
    }
    for (int m = 0; m < 3; m++) { memset(d->mat[m], 0, 64); for (int i = 0; i < 4; i++) d->mat[m][i * 5] = 1.f; }
}

static float fbits(DWORD v) { float f; memcpy(&f, &v, 4); return f; }

static void build_state(d3ddev_t *d, rstate_t *s) {
    memset(s, 0, sizeof *s);
    DWORD *rs = d->rs;
    s->zenable = rs[7] != 0 && d->rt && d->rt->zbuf;
    s->zwrite = rs[14] != 0;
    s->zfunc = rs[23] ? (int)rs[23] : 4;
    s->cull = rs[22] ? (int)rs[22] : 1;
    s->blend = rs[27] != 0;
    s->srcblend = rs[19]; s->dstblend = rs[20];
    s->atest = rs[15] != 0; s->afunc = rs[25] ? (int)rs[25] : 8; s->aref = rs[24] & 255;
    s->fog = rs[28] != 0;
    s->fogtable = rs[35];
    s->fogcolor = rs[34];
    s->fogstart = fbits(rs[36]); s->fogend = fbits(rs[37]); s->fogdensity = fbits(rs[38]);
    s->flat = rs[9] == 1;
    s->specular = rs[29] != 0;
    s->ckey = rs[41] != 0;
    s->dither = rs[26] != 0;
    s->tfactor = rs[60];
    s->zbias = (float)rs[47];
    int n = 0;
    for (int i = 0; i < 2; i++) {
        DWORD *t = d->tss[i];
        if (t[1] == 1) break;   /* COLOROP_DISABLE ends the cascade */
        rstage_t *st = &s->st[i];
        ddsurf_t *tx = d->tex[i];
        st->tex = tex_get(tx);
        st->has_alpha = tx && ((tx->pf.flags & DDPF_ALPHAPIXELS) || tx->has_ck_src);
        st->has_ckey = tx && tx->has_ck_src;
        st->colorop = t[1]; st->carg1 = t[2]; st->carg2 = t[3];
        st->alphaop = t[4]; st->aarg1 = t[5]; st->aarg2 = t[6];
        st->texcoord = t[11] & 7;
        st->addru = t[13] ? (int)t[13] : 1; st->addrv = t[14] ? (int)t[14] : 1;
        st->border = t[15];
        st->magf = t[16] >= 2 ? 2 : 1;
        st->minf = t[17] >= 2 ? 2 : 1;
        st->mipf = t[18] ? (int)t[18] : 1;
        st->lodbias = fbits(t[19]);
        n++;
    }
    s->nstages = n;
    s->vx = d->vp.x; s->vy = d->vp.y; s->vw = d->vp.w; s->vh = d->vp.h;
    s->minz = d->vp.minz; s->maxz = d->vp.maxz;
}

/* ---- FVF decoding ---- */
static int fvf_size(DWORD fvf, int *off_diff, int *off_spec, int *off_tex, int texsz[8]) {
    int sz = 0;
    switch (fvf & 0xe) {
    case 0x2: sz = 12; break;            /* XYZ */
    case 0x4: sz = 16; break;            /* XYZRHW */
    case 0x6: sz = 16; break; case 0x8: sz = 20; break; case 0xa: sz = 24; break; case 0xc: sz = 28; break; case 0xe: sz = 32; break;
    }
    if (fvf & 0x10) sz += 12;            /* NORMAL */
    if (fvf & 0x20) sz += 4;             /* PSIZE / RESERVED1 */
    *off_diff = -1; *off_spec = -1;
    if (fvf & 0x40) { *off_diff = sz; sz += 4; }
    if (fvf & 0x80) { *off_spec = sz; sz += 4; }
    int nt = (fvf >> 8) & 15;
    *off_tex = sz;
    for (int i = 0; i < 8; i++) {
        int f = (fvf >> (16 + i * 2)) & 3;
        texsz[i] = f == 0 ? 2 : f == 1 ? 3 : f == 2 ? 4 : 1;
        if (i < nt) sz += texsz[i] * 4;
    }
    return sz;
}
static void mat_mul(const float *a, const float *b, float *o) {
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c]; o[r * 4 + c] = s;
    }
}
static void decode_verts(d3ddev_t *d, DWORD fvf, const uint8_t *src, int stride, int n, rvert_t *out) {
    int od, os, ot, ts[8];
    int sz = fvf_size(fvf, &od, &os, &ot, ts);
    if (!stride) stride = sz;
    int nt = (fvf >> 8) & 15;
    float W = d->rt ? (float)d->rt->w : 640.f, H = d->rt ? (float)d->rt->h : 480.f;
    int tl = (fvf & 0xe) == 0x4;
    float wvp[16];
    if (!tl) { float t[16]; mat_mul(d->mat[0], d->mat[1], t); mat_mul(t, d->mat[2], wvp); }
    for (int i = 0; i < n; i++, src += stride) {
        const float *p = (const float *)src;
        rvert_t *v = &out[i];
        if (tl) {
            float rhw = p[3];
            float w = (rhw > 1e-8f && isfinite(rhw)) ? 1.f / rhw : 1.f;
            float x = ((p[0] + 0.5f) / W) * 2.f - 1.f;
            float y = 1.f - ((p[1] + 0.5f) / H) * 2.f;
            float z = p[2];
            v->x = x * w; v->y = y * w; v->z = z * w; v->w = w;
        } else {
            float x = p[0], y = p[1], z = p[2];
            float cx = x * wvp[0] + y * wvp[4] + z * wvp[8] + wvp[12];
            float cy = x * wvp[1] + y * wvp[5] + z * wvp[9] + wvp[13];
            float cz = x * wvp[2] + y * wvp[6] + z * wvp[10] + wvp[14];
            float cw = x * wvp[3] + y * wvp[7] + z * wvp[11] + wvp[15];
            /* viewport mapping relative to the render target */
            float sx = d->vp.w / W, sy = d->vp.h / H;
            float ox = (2.f * d->vp.x + d->vp.w) / W - 1.f, oy = 1.f - (2.f * d->vp.y + d->vp.h) / H;
            v->x = cx * sx + ox * cw; v->y = cy * sy + oy * cw;
            v->z = cz * (d->vp.maxz - d->vp.minz) + d->vp.minz * cw; v->w = cw;
        }
        v->diffuse = od >= 0 ? *(const uint32_t *)(src + od) : 0xffffffffu;
        v->specular = os >= 0 ? *(const uint32_t *)(src + os) : 0xff000000u;
        const float *t = (const float *)(src + ot);
        for (int k = 0; k < 2; k++) {
            if (k < nt) { v->uv[k][0] = t[0]; v->uv[k][1] = t[1]; t += ts[k]; }
            else { v->uv[k][0] = v->uv[k][1] = 0; }
        }
    }
}

/* convert any primitive type into a list for the backend */
/* per-draw scratch, grow-only: heap calls per draw are very slow under a debugger (GPxPatch starts
 * GP3 as one, so Windows switches to the debug heap) */
static void *grow(void **buf, size_t *cap, size_t need) {
    if (need > *cap) { size_t c = *cap ? *cap : 4096; while (c < need) c *= 2; free(*buf); *buf = malloc(c); *cap = c; }
    return *buf;
}
static void *g_idx_buf, *g_rv_buf, *g_str_buf; static size_t g_idx_cap, g_rv_cap, g_str_cap;
static int build_list(int type, int n, const uint16_t *idx, uint16_t **out, int *outprim) {
    #define IX(i) (idx ? idx[i] : (uint16_t)(i))
    #define malloc(x) grow(&g_idx_buf, &g_idx_cap, (size_t)(x))
    uint16_t *o = NULL; int m = 0;
    switch (type) {
    case 1: *outprim = RPRIM_POINTS; o = malloc(n * 2); for (int i = 0; i < n; i++) o[m++] = IX(i); break;
    case 2: *outprim = RPRIM_LINES; o = malloc(n * 2); for (int i = 0; i + 1 < n; i += 2) { o[m++] = IX(i); o[m++] = IX(i + 1); } break;
    case 3: *outprim = RPRIM_LINES; o = malloc(n * 4 + 4); for (int i = 0; i + 1 < n; i++) { o[m++] = IX(i); o[m++] = IX(i + 1); } break;
    case 4: *outprim = RPRIM_TRIS; o = malloc(n * 2); for (int i = 0; i + 2 < n; i += 3) { o[m++] = IX(i); o[m++] = IX(i + 1); o[m++] = IX(i + 2); } break;
    case 5: *outprim = RPRIM_TRIS; o = malloc(n * 6 + 6);
        for (int i = 0; i + 2 < n; i++) { if (i & 1) { o[m++] = IX(i + 1); o[m++] = IX(i); } else { o[m++] = IX(i); o[m++] = IX(i + 1); } o[m++] = IX(i + 2); } break;
    case 6: *outprim = RPRIM_TRIS; o = malloc(n * 6 + 6); for (int i = 1; i + 1 < n; i++) { o[m++] = IX(0); o[m++] = IX(i); o[m++] = IX(i + 1); } break;
    default: *out = NULL; return 0;
    }
    #undef IX
    #undef malloc
    *out = o;
    return m;
}

static uint64_t g_draws, g_tris;
int g_spray_opacity = 100;     /* percent, gp3rt.ini [video] spray_opacity */
int g_spray_near_fade = 1;     /* gp3rt.ini [video] spray_near_fade */
static uint32_t g_spray_draws;
static void do_draw(d3ddev_t *d, int type, DWORD fvf, const void *verts, int stride, int nv, const uint16_t *idx, int ni) {
    if (!d->rt || nv <= 0) return;
    int count = idx ? ni : nv;
    if (count <= 0) return;
    uint64_t tf0 = rt_now_us();
    { extern void ddraw_run_deferred(void); ddraw_run_deferred(); }
    uint64_t tf1 = rt_now_us(); g_t_deferred += tf1 - tf0;
    DDLOCK();
    uint64_t tf2 = rt_now_us(); g_t_lockwait += tf2 - tf1;
    rtarget_t *t = gpu_need(d->rt);
    uint64_t tf3 = rt_now_us(); g_t_gpuneed += tf3 - tf2;
    rstate_t st; build_state(d, &st);
    rvert_t *rv = grow(&g_rv_buf, &g_rv_cap, sizeof(rvert_t) * nv);
    decode_verts(d, fvf, verts, stride, nv, rv);
    uint64_t tf4 = rt_now_us(); g_t_prep += tf4 - tf3;
    /* spray / smoke clouds: 64x64 alpha puffs blended SRCALPHA/INVSRCALPHA with texture*diffuse alpha.
     * Near trackside TV cameras a dozen of them pile up into a grey wall; optional thinning. */
    if ((g_spray_opacity < 100 || g_spray_near_fade) && st.blend && st.srcblend == 5 && st.dstblend == 6 && st.nstages == 1 &&
        st.st[0].tex && st.st[0].has_alpha && st.st[0].alphaop == 4 && d->tex[0] && d->tex[0]->w == 64 && d->tex[0]->h == 64) {
        float k = g_spray_opacity / 100.f;
        if (g_spray_near_fade && nv <= 8 && d->rt) {
            /* a puff passing right in front of a trackside camera covers the whole screen: fade it with its size */
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
            for (int i = 0; i < nv; i++) {
                float sx = rv[i].x / rv[i].w, sy = rv[i].y / rv[i].w;
                if (sx < x0) x0 = sx; if (sx > x1) x1 = sx; if (sy < y0) y0 = sy; if (sy > y1) y1 = sy;
            }
            float size = (x1 - x0) * 0.5f > (y1 - y0) * 0.5f ? (x1 - x0) * 0.5f : (y1 - y0) * 0.5f;   /* fraction of the screen */
            if (size > 0.7f) { float f = 1.f - (size - 0.7f) / 0.9f; k *= f < 0 ? 0 : f; }
        }
        if (k < 0.999f) {
            for (int i = 0; i < nv; i++) {
                uint32_t a = (uint32_t)((rv[i].diffuse >> 24) * k + 0.5f);
                rv[i].diffuse = (rv[i].diffuse & 0xffffff) | a << 24;
            }
            g_spray_draws++;
        }
    }
    uint16_t *list; int prim;
    int m = build_list(type, count, idx, &list, &prim);
    uint64_t td = rt_now_us(); g_t_list += td - tf4;
    if (m > 0) RDR->draw(t, &st, prim, rv, nv, list, m);
    g_t_draw += rt_now_us() - td;
    g_draws++; g_st.draws++; if (prim == RPRIM_TRIS) g_tris += m / 3;
    gpu_written(d->rt);
    DDUNLOCK();
    g_t_tail += rt_now_us() - td;
    g_t_front += rt_now_us() - tf0;
}

void d3d_depth_fill(ddsurf_t *z, RECT r, uint32_t v) {
    if (!g_dev || !g_dev->rt || g_dev->rt->zbuf != z) return;
    sx_t *x = sx_of(g_dev->rt);
    if (!x->tgt) return;
    int rect[4] = { r.left, r.top, r.right, r.bottom };
    float zf = z->bpp == 16 ? v / 65535.f : v / 16777215.f;
    RDR->clear(x->tgt, rect, 1, 0, 0, 1, zf, 0, 0, g_dev->rt->w, g_dev->rt->h);
}

/* ------------------------------------------------------------------ */
/* IDirect3DDevice7 methods                                             */
#define THIS d3ddev_t *This
static HRESULT WINAPI dev_QueryInterface(THIS, const uint8_t *iid, void **out) { API_NOTE("dev_QueryInterface");
    extern int guid_eq(const uint8_t *g, const char *s); extern void guid_str(const uint8_t *g, char *o);
    if (!guid_eq(iid, "F5049E79-4861-11D2-A407-00A0C90629A8") && !guid_eq(iid, "00000000-0000-0000-C000-000000000046")) {
        char g[64]; guid_str(iid, g); LOG("Direct3DDevice::QueryInterface(%s): answering with the device", g);
    }
    *out = This; This->refs++; return 0;
}
static DWORD WINAPI dev_AddRef(THIS) { API_NOTE("dev_AddRef"); return ++This->refs; }
static DWORD WINAPI dev_Release(THIS) { API_NOTE("dev_Release");
    DWORD r = --This->refs;
    if (!r) {
        LOG("D3D device released (draws=%llu tris=%llu)", (unsigned long long)g_draws, (unsigned long long)g_tris);
        for (int i = 0; i < 8; i++) if (This->tex[i]) This->tex[i]->refs--;
        if (This->rt) This->rt->refs--;
        if (g_dev == This) g_dev = NULL;
        free(This);
    }
    return r;
}
static void fill_primcaps(DWORD *p) {
    p[0] = 56;
    p[1] = 0x2 | 0x10 | 0x20 | 0x40;                      /* misc: MASKZ, CULLNONE/CW/CCW */
    p[2] = 0x1 | 0x10 | 0x80 | 0x100 | 0x2000 | 0x4000 | 0x10000 | 0x20000 | 0x100000 | 0x200000; /* raster */
    p[3] = 0xff;                                           /* zcmp */
    p[4] = 0x1fff; p[5] = 0x1fff;                          /* blend */
    p[6] = 0xff;                                           /* alpha cmp */
    p[7] = 0x8 | 0x200 | 0x4000 | 0x80000;                 /* shade */
    p[8] = 0x1 | 0x2 | 0x4 | 0x8 | 0x80 | 0x2000;          /* texture: perspective, POW2 (GP3 computes UVs for power-of-two pages), alpha, transparency, alphapalette */
    p[9] = 0x3f | 0x100 | 0x200 | 0x10000 | 0x20000 | 0x1000000 | 0x2000000;  /* filter */
    p[10] = 0xff;                                          /* legacy blend */
    p[11] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10;                  /* address */
}
static void fill_devdesc(uint8_t *dd, int tnl, const char *guid) {
    memset(dd, 0, 236);
    DWORD *d = (DWORD *)dd;
    d[0] = 0x1 | 0x10 | 0x40 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x8000 | 0x20000 | 0x80000 | (tnl ? 0x10000 : 0);
    fill_primcaps((DWORD *)(dd + 4));
    fill_primcaps((DWORD *)(dd + 60));
    *(DWORD *)(dd + 116) = 0x400 | 0x100;           /* render depth 16/32 */
    *(DWORD *)(dd + 120) = 0x400 | 0x200 | 0x100;   /* z depth 16/24/32 */
    *(DWORD *)(dd + 124) = 1; *(DWORD *)(dd + 128) = 1;
    *(DWORD *)(dd + 132) = 2048; *(DWORD *)(dd + 136) = 2048;
    *(DWORD *)(dd + 140) = 8192; *(DWORD *)(dd + 144) = 2048; *(DWORD *)(dd + 148) = 16;
    float gb[4] = { -4096.f, -4096.f, 4096.f, 4096.f }; memcpy(dd + 152, gb, 16);
    *(DWORD *)(dd + 172) = 0xff;                    /* stencil caps */
    *(DWORD *)(dd + 176) = 8;                       /* FVF caps: 8 tex coords */
    *(DWORD *)(dd + 180) = 0x3ffffff;               /* texture op caps */
    *(WORD *)(dd + 184) = 8; *(WORD *)(dd + 186) = 2;
    *(DWORD *)(dd + 188) = 8;
    float mw = 1e10f; memcpy(dd + 192, &mw, 4);
    unsigned a, b, c, e[8];
    sscanf(guid, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c, &e[0], &e[1], &e[2], &e[3], &e[4], &e[5], &e[6], &e[7]);
    memcpy(dd + 196, &a, 4); WORD bb = b, cc = c; memcpy(dd + 200, &bb, 2); memcpy(dd + 202, &cc, 2);
    for (int i = 0; i < 8; i++) dd[204 + i] = (uint8_t)e[i];
    *(DWORD *)(dd + 216) = tnl ? 0x3f : 0;
}
static HRESULT WINAPI dev_GetCaps(THIS, uint8_t *desc) { API_NOTE("dev_GetCaps");
    fill_devdesc(desc, This->is_tnl, This->is_tnl ? IID_IDirect3DTnLHalDevice : IID_IDirect3DHALDevice);
    return 0;
}
typedef HRESULT (WINAPI *pf_cb)(DDPIXELFORMAT *, void *);
static HRESULT WINAPI dev_EnumTextureFormats(THIS, pf_cb cb, void *ctx) { API_NOTE("dev_EnumTextureFormats");
    (void)This;
    DDPIXELFORMAT f[6]; memset(f, 0, sizeof f);
    for (int i = 0; i < 6; i++) f[i].size = 32;
    f[0].flags = DDPF_RGB; f[0].bpp = 16; f[0].rmask = 0xf800; f[0].gmask = 0x7e0; f[0].bmask = 0x1f;
    f[1].flags = DDPF_RGB | DDPF_ALPHAPIXELS; f[1].bpp = 16; f[1].rmask = 0x7c00; f[1].gmask = 0x3e0; f[1].bmask = 0x1f; f[1].amask = 0x8000;
    f[2].flags = DDPF_RGB | DDPF_ALPHAPIXELS; f[2].bpp = 16; f[2].rmask = 0xf00; f[2].gmask = 0xf0; f[2].bmask = 0xf; f[2].amask = 0xf000;
    f[3].flags = DDPF_RGB; f[3].bpp = 16; f[3].rmask = 0x7c00; f[3].gmask = 0x3e0; f[3].bmask = 0x1f;
    f[4].flags = DDPF_RGB | DDPF_ALPHAPIXELS; f[4].bpp = 32; f[4].rmask = 0xff0000; f[4].gmask = 0xff00; f[4].bmask = 0xff; f[4].amask = 0xff000000;
    f[5].flags = DDPF_RGB; f[5].bpp = 32; f[5].rmask = 0xff0000; f[5].gmask = 0xff00; f[5].bmask = 0xff;
    for (int i = 0; i < 6; i++) if (cb(&f[i], ctx) == 0) break;
    return 0;
}
static HRESULT WINAPI dev_BeginScene(THIS) { API_NOTE("dev_BeginScene"); This->in_scene = 1; return 0; }
static HRESULT WINAPI dev_EndScene(THIS) { API_NOTE("dev_EndScene"); This->in_scene = 0; return 0; }
static HRESULT WINAPI dev_GetDirect3D(THIS, void **o) { API_NOTE("dev_GetDirect3D"); *o = This->d3d; This->d3d->refs++; return 0; }
static HRESULT WINAPI dev_SetRenderTarget(THIS, ddsurf_t *s, DWORD f) { API_NOTE("dev_SetRenderTarget");
    (void)f;
    if (!s) return DDERR_INVALIDPARAMS;
    DTRACE("SetRenderTarget(%p %dx%d)", (void *)s, s->w, s->h);
    s->refs++;
    if (This->rt) This->rt->refs--;
    This->rt = s; g_render_target = s;
    This->vp = (D3DVIEWPORT7){ 0, 0, (DWORD)s->w, (DWORD)s->h, 0.f, 1.f };
    return 0;
}
static HRESULT WINAPI dev_GetRenderTarget(THIS, ddsurf_t **s) { API_NOTE("dev_GetRenderTarget"); *s = This->rt; if (This->rt) This->rt->refs++; return 0; }
static HRESULT WINAPI dev_Clear(THIS, DWORD n, const LONG *rects, DWORD flags, DWORD color, float z, DWORD stencil) { API_NOTE("dev_Clear");
    (void)stencil;
    if (!This->rt) return 0;
    DDLOCK();
    rtarget_t *t = gpu_need(This->rt);
    static int *cr; static size_t ccap; int *r = NULL;
    if (n && rects) { r = grow((void **)&cr, &ccap, n * 16); memcpy(r, rects, n * 16); }
    RDR->clear(t, r, r ? (int)n : 0, (flags & 1) != 0, color, (flags & 2) != 0, z, This->vp.x, This->vp.y, This->vp.w, This->vp.h);
    if (flags & 1) gpu_written(This->rt);
    DDUNLOCK();
    return 0;
}
static HRESULT WINAPI dev_SetTransform(THIS, DWORD type, const float *m) { API_NOTE("dev_SetTransform");
    int i = type == 1 ? 1 : type == 2 ? 0 : type == 3 ? 2 : (type == 256 ? 0 : -1);
    if (i >= 0) memcpy(This->mat[i], m, 64);
    return 0;
}
static HRESULT WINAPI dev_GetTransform(THIS, DWORD type, float *m) { API_NOTE("dev_GetTransform");
    int i = type == 1 ? 1 : type == 2 ? 0 : type == 3 ? 2 : (type == 256 ? 0 : -1);
    if (i >= 0) memcpy(m, This->mat[i], 64); else memset(m, 0, 64);
    return 0;
}
static HRESULT WINAPI dev_SetViewport(THIS, const D3DVIEWPORT7 *vp) { API_NOTE("dev_SetViewport");
    This->vp = *vp;
    DTRACE("SetViewport(%u,%u %ux%u z %.2f-%.2f)", vp->x, vp->y, vp->w, vp->h, vp->minz, vp->maxz);
    return 0;
}
static HRESULT WINAPI dev_MultiplyTransform(THIS, DWORD type, const float *m) { API_NOTE("dev_MultiplyTransform");
    int i = type == 1 ? 1 : type == 2 ? 0 : type == 3 ? 2 : -1;
    if (i >= 0) { float o[16]; mat_mul(m, This->mat[i], o); memcpy(This->mat[i], o, 64); }
    return 0;
}
static HRESULT WINAPI dev_GetViewport(THIS, D3DVIEWPORT7 *vp) { API_NOTE("dev_GetViewport"); *vp = This->vp; return 0; }
static HRESULT WINAPI dev_SetMaterial(THIS, const void *m) { API_NOTE("dev_SetMaterial"); memcpy(This->material, m, 68); return 0; }
static HRESULT WINAPI dev_GetMaterial(THIS, void *m) { API_NOTE("dev_GetMaterial"); memcpy(m, This->material, 68); return 0; }
static HRESULT WINAPI dev_SetLight(THIS, DWORD i, const void *l) { API_NOTE("dev_SetLight"); if (i < 16) memcpy(This->lights[i], l, 104); return 0; }
static HRESULT WINAPI dev_GetLight(THIS, DWORD i, void *l) { API_NOTE("dev_GetLight"); if (i < 16) memcpy(l, This->lights[i], 104); return 0; }
static HRESULT WINAPI dev_SetRenderState(THIS, DWORD st, DWORD v) { API_NOTE("dev_SetRenderState");
    if (st >= 256) return DDERR_INVALIDPARAMS;
    if (This->recording) {   /* recorded into the block, not applied (D3D7 semantics) */
        if (This->rec) { This->rec->rs[st] = v; This->rec->rs_m[st] = 1; This->rec->valid = 1; }
        return 0;
    }
    if (This->rs[st] != v) DTRACE("SetRenderState(%u, %x)", st, v);
    This->rs[st] = v;
    return 0;
}
static HRESULT WINAPI dev_GetRenderState(THIS, DWORD st, DWORD *v) { API_NOTE("dev_GetRenderState"); *v = st < 256 ? This->rs[st] : 0; return 0; }
static HRESULT WINAPI dev_BeginStateBlock(THIS) { API_NOTE("dev_BeginStateBlock");
    if (This->recording) return 0x887602f4;   /* D3DERR_INBEGINSTATEBLOCK */
    This->recording = 1; This->rec = calloc(1, sizeof(sblock_t));   /* empty: only what is set while recording */
    return 0;
}
static HRESULT WINAPI dev_EndStateBlock(THIS, DWORD *h) { API_NOTE("dev_EndStateBlock");
    This->recording = 0;
    for (int i = 1; i < 256; i++) if (!g_sblocks[i]) { g_sblocks[i] = This->rec; This->rec = NULL; *h = i; return 0; }
    free(This->rec); This->rec = NULL; *h = 0;
    return DDERR_OUTOFMEMORY;
}
static HRESULT WINAPI dev_PreLoad(THIS, ddsurf_t *s) { API_NOTE("dev_PreLoad"); (void)This; DDLOCK(); tex_get(s); DDUNLOCK(); return 0; }
static HRESULT WINAPI dev_DrawPrimitive(THIS, DWORD type, DWORD fvf, const void *v, DWORD n, DWORD f) { API_NOTE("dev_DrawPrimitive");
    (void)f;
    do_draw(This, (int)type, fvf, v, 0, (int)n, NULL, 0);
    return 0;
}
static HRESULT WINAPI dev_DrawIndexedPrimitive(THIS, DWORD type, DWORD fvf, const void *v, DWORD nv, const uint16_t *idx, DWORD ni, DWORD f) { API_NOTE("dev_DrawIndexedPrimitive");
    (void)f;
    do_draw(This, (int)type, fvf, v, 0, (int)nv, idx, (int)ni);
    return 0;
}
static HRESULT WINAPI dev_SetClipStatus(THIS, const void *c) { API_NOTE("dev_SetClipStatus"); memcpy(This->clipstatus, c, 24); return 0; }
static HRESULT WINAPI dev_GetClipStatus(THIS, void *c) { API_NOTE("dev_GetClipStatus"); memcpy(c, This->clipstatus, 24); return 0; }

/* strided: gather into a packed buffer */
static void *gather_strided(DWORD fvf, const uint32_t *sd, int nv, int *stride_out) {
    int od, os, ot, ts[8];
    int sz = fvf_size(fvf, &od, &os, &ot, ts);
    uint8_t *buf = grow(&g_str_buf, &g_str_cap, (size_t)nv * sz); memset(buf, 0, (size_t)nv * sz);
    int psz = (fvf & 0xe) == 0x4 ? 16 : 12;
    int nt = (fvf >> 8) & 15;
    for (int i = 0; i < nv; i++) {
        uint8_t *o = buf + i * sz;
        memcpy(o, (const uint8_t *)(uintptr_t)sd[0] + i * sd[1], psz);
        if (od >= 0) memcpy(o + od, (const uint8_t *)(uintptr_t)sd[4] + i * sd[5], 4);
        if (os >= 0) memcpy(o + os, (const uint8_t *)(uintptr_t)sd[6] + i * sd[7], 4);
        int off = ot;
        for (int t = 0; t < nt && t < 8; t++) { memcpy(o + off, (const uint8_t *)(uintptr_t)sd[8 + t * 2] + i * sd[9 + t * 2], ts[t] * 4); off += ts[t] * 4; }
    }
    *stride_out = sz;
    return buf;
}
static HRESULT WINAPI dev_DrawPrimitiveStrided(THIS, DWORD type, DWORD fvf, const uint32_t *sd, DWORD n, DWORD f) { API_NOTE("dev_DrawPrimitiveStrided");
    (void)f; int st; void *b = gather_strided(fvf, sd, (int)n, &st);
    do_draw(This, (int)type, fvf, b, st, (int)n, NULL, 0); return 0;
}
static HRESULT WINAPI dev_DrawIndexedPrimitiveStrided(THIS, DWORD type, DWORD fvf, const uint32_t *sd, DWORD nv, const uint16_t *idx, DWORD ni, DWORD f) { API_NOTE("dev_DrawIndexedPrimitiveStrided");
    (void)f; int st; void *b = gather_strided(fvf, sd, (int)nv, &st);
    do_draw(This, (int)type, fvf, b, st, (int)nv, idx, (int)ni); return 0;
}

typedef struct { void **vtbl; LONG refs; DWORD caps, fvf, n; uint8_t *data; int stride; } d3dvb_t;
static HRESULT WINAPI dev_DrawPrimitiveVB(THIS, DWORD type, d3dvb_t *vb, DWORD start, DWORD n, DWORD f) { API_NOTE("dev_DrawPrimitiveVB");
    (void)f;
    do_draw(This, (int)type, vb->fvf, vb->data + start * vb->stride, vb->stride, (int)n, NULL, 0);
    return 0;
}
static HRESULT WINAPI dev_DrawIndexedPrimitiveVB(THIS, DWORD type, d3dvb_t *vb, DWORD start, DWORD nv, const uint16_t *idx, DWORD ni, DWORD f) { API_NOTE("dev_DrawIndexedPrimitiveVB");
    (void)f;
    do_draw(This, (int)type, vb->fvf, vb->data + start * vb->stride, vb->stride, (int)nv, idx, (int)ni);
    return 0;
}
static HRESULT WINAPI dev_ComputeSphereVisibility(THIS, void *c, void *r, DWORD n, DWORD f, DWORD *ret) { API_NOTE("dev_ComputeSphereVisibility");
    (void)This; (void)c; (void)r; (void)f; for (DWORD i = 0; i < n; i++) ret[i] = 0; return 0;
}
static HRESULT WINAPI dev_GetTexture(THIS, DWORD st, ddsurf_t **o) { API_NOTE("dev_GetTexture"); *o = st < 8 ? This->tex[st] : NULL; if (*o) (*o)->refs++; return 0; }
static HRESULT WINAPI dev_SetTexture(THIS, DWORD st, ddsurf_t *s) { API_NOTE("dev_SetTexture");
    if (st >= 8) return DDERR_INVALIDPARAMS;
    if (This->recording) {
        if (This->rec) { This->rec->tex[st] = s; This->rec->tex_m[st] = 1; This->rec->valid = 1; }
        return 0;
    }
    if (s) s->refs++;
    if (This->tex[st]) This->tex[st]->refs--;
    This->tex[st] = s;
    return 0;
}
static HRESULT WINAPI dev_GetTextureStageState(THIS, DWORD st, DWORD t, DWORD *v) { API_NOTE("dev_GetTextureStageState"); *v = (st < 8 && t < 32) ? This->tss[st][t] : 0; return 0; }
static HRESULT WINAPI dev_SetTextureStageState(THIS, DWORD st, DWORD t, DWORD v) { API_NOTE("dev_SetTextureStageState");
    if (st >= 8 || t >= 32) return DDERR_INVALIDPARAMS;
    if (This->recording) {
        if (This->rec) {
            sblock_t *b = This->rec; b->valid = 1;
            b->tss[st][t] = v; b->tss_m[st][t] = 1;
            if (t == 12) { b->tss[st][13] = b->tss[st][14] = v; b->tss_m[st][13] = b->tss_m[st][14] = 1; }
        }
        return 0;
    }
    if (t == 12) { This->tss[st][13] = This->tss[st][14] = v; }   /* ADDRESS sets U and V */
    if (This->tss[st][t] != v) DTRACE("SetTextureStageState(%u, %u, %x)", st, t, v);
    This->tss[st][t] = v;
    return 0;
}
static HRESULT WINAPI dev_ValidateDevice(THIS, DWORD *passes) { API_NOTE("dev_ValidateDevice"); (void)This; *passes = 1; return 0; }
static HRESULT WINAPI dev_ApplyStateBlock(THIS, DWORD h) { API_NOTE("dev_ApplyStateBlock");
    if (h >= 256 || !g_sblocks[h]) return DDERR_INVALIDPARAMS;
    if (This->recording) return 0x887602f4;
    sblock_t *b = g_sblocks[h];
    for (int i = 0; i < 256; i++) if (b->rs_m[i]) This->rs[i] = b->rs[i];
    for (int st = 0; st < 8; st++) for (int t = 0; t < 32; t++) if (b->tss_m[st][t]) This->tss[st][t] = b->tss[st][t];
    for (int i = 0; i < 8; i++) if (b->tex_m[i]) dev_SetTexture(This, i, b->tex[i]);
    return 0;
}
static HRESULT WINAPI dev_CaptureStateBlock(THIS, DWORD h) { API_NOTE("dev_CaptureStateBlock");
    if (h >= 256 || !g_sblocks[h]) return DDERR_INVALIDPARAMS;
    if (This->recording) return 0x887602f4;
    sblock_t *b = g_sblocks[h];   /* refresh only the states that belong to the block */
    for (int i = 0; i < 256; i++) if (b->rs_m[i]) b->rs[i] = This->rs[i];
    for (int st = 0; st < 8; st++) for (int t = 0; t < 32; t++) if (b->tss_m[st][t]) b->tss[st][t] = This->tss[st][t];
    for (int i = 0; i < 8; i++) if (b->tex_m[i]) b->tex[i] = This->tex[i];
    return 0;
}
static HRESULT WINAPI dev_DeleteStateBlock(THIS, DWORD h) { API_NOTE("dev_DeleteStateBlock"); (void)This; if (h < 256 && g_sblocks[h]) { free(g_sblocks[h]); g_sblocks[h] = NULL; } return 0; }
static HRESULT WINAPI dev_CreateStateBlock(THIS, DWORD type, DWORD *h) { API_NOTE("dev_CreateStateBlock");
    (void)type;   /* ALL / PIXELSTATE / VERTEXSTATE: all states this implementation keeps */
    sblock_t *b = calloc(1, sizeof *b);
    memcpy(b->rs, This->rs, sizeof This->rs); memcpy(b->tss, This->tss, sizeof This->tss); memcpy(b->tex, This->tex, sizeof This->tex);
    memset(b->rs_m, 1, sizeof b->rs_m); memset(b->tss_m, 1, sizeof b->tss_m); memset(b->tex_m, 1, sizeof b->tex_m);
    for (int i = 1; i < 256; i++) if (!g_sblocks[i]) { g_sblocks[i] = b; *h = i; return 0; }
    free(b); return DDERR_OUTOFMEMORY;
}
static HRESULT WINAPI dev_Load(THIS, ddsurf_t *dst, const LONG *pt, ddsurf_t *src, const RECT *r, DWORD f) { API_NOTE("dev_Load");
    (void)This; (void)pt; (void)r; (void)f;
    DDLOCK();
    for (ddsurf_t *d = dst, *s = src; d && s; d = d->mip_next, s = s->mip_next) {
        cpu_need(s); cpu_need(d);
        if (d->w == s->w && d->h == s->h && d->bpp == s->bpp)
            for (int y = 0; y < s->h; y++) memcpy(d->bits + (ptrdiff_t)y * d->pitch, s->bits + (ptrdiff_t)y * s->pitch, (size_t)s->w * ((s->bpp + 7) / 8));
        if (s->pal && !d->pal) { d->pal = s->pal; s->pal->refs++; }
        d->version++; cpu_written(d);
    }
    DDUNLOCK();
    return 0;
}
static HRESULT WINAPI dev_LightEnable(THIS, DWORD i, BOOL e) { API_NOTE("dev_LightEnable"); if (i < 16) This->light_on[i] = e; return 0; }
static HRESULT WINAPI dev_GetLightEnable(THIS, DWORD i, BOOL *e) { API_NOTE("dev_GetLightEnable"); *e = i < 16 ? This->light_on[i] : 0; return 0; }
static HRESULT WINAPI dev_SetClipPlane(THIS, DWORD i, const float *p) { API_NOTE("dev_SetClipPlane"); (void)This; (void)i; (void)p; return 0; }
static HRESULT WINAPI dev_GetClipPlane(THIS, DWORD i, float *p) { API_NOTE("dev_GetClipPlane"); (void)This; (void)i; memset(p, 0, 16); return 0; }
static HRESULT WINAPI dev_GetInfo(THIS, DWORD id, void *p, DWORD n) { API_NOTE("dev_GetInfo"); (void)This; (void)id; memset(p, 0, n); return 1; }

/* ------------------------------------------------------------------ */
/* vertex buffers                                                       */
static HRESULT WINAPI vb_QueryInterface(d3dvb_t *This, const uint8_t *iid, void **o) { API_NOTE("vb_QueryInterface"); (void)iid; *o = This; This->refs++; return 0; }
static DWORD WINAPI vb_AddRef(d3dvb_t *This) { API_NOTE("vb_AddRef"); return ++This->refs; }
static DWORD WINAPI vb_Release(d3dvb_t *This) { API_NOTE("vb_Release"); DWORD r = --This->refs; if (!r) { free(This->data); free(This); } return r; }
static HRESULT WINAPI vb_Lock(d3dvb_t *This, DWORD f, void **p, DWORD *sz) { API_NOTE("vb_Lock"); (void)f; *p = This->data; if (sz) *sz = This->n * This->stride; return 0; }
static HRESULT WINAPI vb_Unlock(d3dvb_t *This) { API_NOTE("vb_Unlock"); (void)This; return 0; }
static HRESULT WINAPI vb_ProcessVertices(d3dvb_t *This, DWORD op, DWORD di, DWORD n, d3dvb_t *src, DWORD si, void *dev, DWORD f) { API_NOTE("vb_ProcessVertices");
    (void)op; (void)dev; (void)f;
    if (src->fvf == This->fvf) memcpy(This->data + di * This->stride, src->data + si * src->stride, (size_t)n * This->stride);
    return 0;
}
static HRESULT WINAPI vb_GetVertexBufferDesc(d3dvb_t *This, DWORD *d) { API_NOTE("vb_GetVertexBufferDesc"); d[1] = This->caps; d[2] = This->fvf; d[3] = This->n; return 0; }
static HRESULT WINAPI vb_Optimize(d3dvb_t *This, void *dev, DWORD f) { API_NOTE("vb_Optimize"); (void)This; (void)dev; (void)f; return 0; }
static HRESULT WINAPI vb_ProcessVerticesStrided(d3dvb_t *This, DWORD op, DWORD di, DWORD n, void *sd, DWORD fvf, void *dev, DWORD f) { API_NOTE("vb_ProcessVerticesStrided");
    (void)This; (void)op; (void)di; (void)n; (void)sd; (void)fvf; (void)dev; (void)f; return DDERR_UNSUPPORTED;
}

/* ------------------------------------------------------------------ */
/* IDirect3D7                                                           */
/* IDirect3D7 is aggregated with the DirectDraw object: D3DX (used by add-ons such as
 * GPxPatch) asks it for IDirectDraw7 to create its textures */
static HRESULT WINAPI d3d_QueryInterface(d3d7_t *This, const uint8_t *iid, void **o) { API_NOTE("d3d_QueryInterface");
    extern int guid_eq(const uint8_t *g, const char *s); extern void guid_str(const uint8_t *g, char *o);
    if (guid_eq(iid, "F5049E77-4861-11D2-A407-00A0C90629A8") || guid_eq(iid, "00000000-0000-0000-C000-000000000046")) {
        *o = This; This->refs++; return 0;
    }
    if (This->dd && (guid_eq(iid, "15E65EC0-3B9C-11D2-B92F-00609797EA5B") || guid_eq(iid, "9C59509A-39BD-11D1-8C4A-00C04FD930C5"))) {
        *o = This->dd; This->dd->refs++;
        LOG("Direct3D::QueryInterface(IDirectDraw7) -> DirectDraw %p", (void *)This->dd);
        return 0;
    }
    char g[64]; guid_str(iid, g);
    LOG("Direct3D::QueryInterface(%s) -> E_NOINTERFACE", g);
    *o = NULL;
    return 0x80004002;
}
static DWORD WINAPI d3d_AddRef(d3d7_t *This) { API_NOTE("d3d_AddRef"); return ++This->refs; }
static DWORD WINAPI d3d_Release(d3d7_t *This) { API_NOTE("d3d_Release"); DWORD r = --This->refs; if (!r) { if (This->dd) This->dd->d3d = NULL; free(This); } return r; }
typedef HRESULT (WINAPI *devenum_cb)(char *desc, char *name, uint8_t *dd, void *ctx);
static HRESULT WINAPI d3d_EnumDevices(d3d7_t *This, devenum_cb cb, void *ctx) { API_NOTE("d3d_EnumDevices");
    (void)This;
    static char n1[] = "RGB Emulation", d1[] = "Microsoft Direct3D RGB Software Emulation";
    static char n2[] = "Direct3D HAL", d2[] = "Microsoft Direct3D Hardware acceleration through Direct3D HAL";
    static char n3[] = "Direct3D T&L HAL", d3[] = "Microsoft Direct3D Hardware Transform and Lighting acceleration capable device";
    uint8_t dd[236];
    fill_devdesc(dd, 0, IID_IDirect3DRGBDevice);
    ((DWORD *)dd)[0] &= ~0x80000u;
    if (cb(d1, n1, dd, ctx) == 0) return 0;
    fill_devdesc(dd, 0, IID_IDirect3DHALDevice);
    if (cb(d2, n2, dd, ctx) == 0) return 0;
    fill_devdesc(dd, 1, IID_IDirect3DTnLHalDevice);
    cb(d3, n3, dd, ctx);
    return 0;
}
static HRESULT WINAPI d3d_CreateDevice(d3d7_t *This, const uint8_t *clsid, ddsurf_t *rt, d3ddev_t **out) { API_NOTE("d3d_CreateDevice");
    d3ddev_t *d = calloc(1, sizeof *d);
    d->vtbl = g_dev_vtbl; d->refs = 1; d->d3d = This; This->refs++;
    d->is_tnl = guid_eq(clsid, IID_IDirect3DTnLHalDevice);
    memcpy(d->guid, clsid, 16);
    d->rt = rt; if (rt) rt->refs++;
    g_render_target = rt;
    d->vp = (D3DVIEWPORT7){ 0, 0, rt ? (DWORD)rt->w : 640, rt ? (DWORD)rt->h : 480, 0.f, 1.f };
    dev_defaults(d);
    g_dev = d;
    *out = d;
    LOG("[thread %u] CreateDevice(%s) rt=%p %dx%dx%d zbuf=%p backend=%s", os_thread_id(),
        d->is_tnl ? "TnLHAL" : guid_eq(clsid, IID_IDirect3DHALDevice) ? "HAL" : "other",
        (void *)rt, rt ? rt->w : 0, rt ? rt->h : 0, rt ? rt->bpp : 0, rt ? (void *)rt->zbuf : NULL, RDR->name);
    d3d_install_shadow_traps();
    return 0;
}
static HRESULT WINAPI d3d_CreateVertexBuffer(d3d7_t *This, const DWORD *desc, d3dvb_t **out, DWORD f) { API_NOTE("d3d_CreateVertexBuffer");
    (void)This; (void)f;
    d3dvb_t *v = calloc(1, sizeof *v);
    v->vtbl = g_vb_vtbl; v->refs = 1; v->caps = desc[1]; v->fvf = desc[2]; v->n = desc[3];
    int a, b, c, t[8]; v->stride = fvf_size(v->fvf, &a, &b, &c, t);
    v->data = calloc(v->n ? v->n : 1, v->stride);
    *out = v;
    return 0;
}
static HRESULT WINAPI d3d_EnumZBufferFormats(d3d7_t *This, const uint8_t *clsid, pf_cb cb, void *ctx) { API_NOTE("d3d_EnumZBufferFormats");
    (void)This; (void)clsid;
    /* z formats: bpp field = z depth, rmask = stencil depth, gmask = z mask, bmask = stencil mask */
    DDPIXELFORMAT f[3]; memset(f, 0, sizeof f);
    for (int i = 0; i < 3; i++) { f[i].size = 32; f[i].flags = DDPF_ZBUFFER; }
    f[0].bpp = 16; f[0].gmask = 0xffff;
    f[1].bpp = 24; f[1].gmask = 0xffffff;
    f[2].bpp = 32; f[2].gmask = 0xffffff; f[2].rmask = 8; f[2].bmask = 0xff000000; f[2].flags |= 0x4000; /* DDPF_STENCILBUFFER */
    for (int i = 0; i < 3; i++) if (cb(&f[i], ctx) == 0) break;
    return 0;
}
static HRESULT WINAPI d3d_EvictManagedTextures(d3d7_t *This) { API_NOTE("d3d_EvictManagedTextures"); (void)This; return 0; }

/* ---- API census (see apinote.h) ---- */
extern uintptr_t plat_module_of(void *addr, char *name, int n);
void api_note(const char *name, void *caller) {
    /* call sites already handled: nothing to do (this runs on every DirectDraw/D3D call) */
    static uintptr_t sites[4096];
    uintptr_t c = (uintptr_t)caller; unsigned h = (unsigned)((c >> 2) * 2654435761u) >> 20;
    for (unsigned k = 0; k < 8; k++) { unsigned i = (h + k) & 4095; if (sites[i] == c) return; if (!sites[i]) { sites[i] = c; break; } }
    static struct { const char *name; uintptr_t mod; } seen[768]; static int nseen;
    char mn[128] = "?";
    uintptr_t mod = plat_module_of(caller, mn, sizeof mn);
    for (int i = 0; i < nseen; i++) if (seen[i].name == name && seen[i].mod == mod) return;
    if (nseen < 768) { seen[nseen].name = name; seen[nseen].mod = mod; nseen++; }
    LOG("API %s first called from %s+0x%x (thread %u)", name, mn, (unsigned)((uintptr_t)caller - mod), os_thread_id());
}

/* choose the renderer backend (platform may have installed one already) */
rbackend_t *(*g_platform_backend)(void);
int g_frame_dump; char g_frame_dump_dir[600];
void rdr_init(void) {
    if (g_rdr) return;
    if (g_platform_backend) g_rdr = g_platform_backend();
    if (!g_rdr) g_rdr = rdr_soft_backend();
    extern char g_capture_path[];
    if (g_capture_path[0]) { extern rbackend_t *rdr_capture_wrap(rbackend_t *, const char *); g_rdr = rdr_capture_wrap(g_rdr, g_capture_path); }
    { extern int g_frame_dump; extern char g_frame_dump_dir[];
      if (g_frame_dump && g_frame_dump_dir[0]) { extern rbackend_t *rdr_framedump_wrap(rbackend_t *, const char *); g_rdr = rdr_framedump_wrap(g_rdr, g_frame_dump_dir); } }
    g_rdr_tid = os_thread_id();
    LOG("renderer: %s (thread %u)", g_rdr->name, g_rdr_tid);
}
/* OpenGL contexts belong to one thread. GP3 sometimes presents (loading screens)
 * from a second thread; those presents are deferred to the owner thread. */
int g_rdr_thread_bound;            /* set by platforms whose backend is thread-affine (WGL) */

/* A WGL context is current on at most one thread. GP3 renders on its game thread
 * but asks its window thread (SendMessage) to switch modes and draw loading
 * screens, so the context is handed over: the platform makes it current on the
 * calling thread when it is free (g_rdr_acquire) and releases it around
 * SendMessage and at the end of window messages. */
int g_rdr_thread_bound;            /* set by platforms whose backend is thread-affine (WGL) */
int (*g_rdr_acquire)(void);        /* 1 = renderer usable on this thread now */
void *g_rdr_caller;
int (*g_rdr_held_elsewhere)(void);   /* platform: context current on another thread right now */
int rdr_held_elsewhere(void) { return g_rdr && g_rdr_thread_bound && g_rdr_held_elsewhere && g_rdr_held_elsewhere(); }
int rdr_foreign_thread(void) {
    if (!g_rdr_thread_bound || !g_rdr || !g_rdr_acquire) return 0;
    g_rdr_caller = __builtin_return_address(0);
    return !g_rdr_acquire();
}
__attribute__((noinline)) static rbackend_t *rdr_get(void) {
    if (!g_rdr) rdr_init();
    else if (g_rdr_thread_bound && g_rdr_acquire && ((g_rdr_caller = __builtin_return_address(0)), !g_rdr_acquire())) {
        /* held by another thread that is running: wait for it to let go */
        LOG("renderer busy on another thread (caller %p): waiting", __builtin_return_address(0));
        int i = 0;
        while (!g_rdr_acquire() && i < 3000) { os_sleep_us(1000); i++; }
        LOG(i < 3000 ? "renderer acquired after %d ms" : "renderer still busy after %d ms: continuing", i);
    }
    unsigned t = os_thread_id();
    if (t != g_rdr_tid && t != g_rdr_other_tid) {
        g_rdr_other_tid = t;
        LOG("renderer used from thread %u (owner %u), caller %p", t, g_rdr_tid, __builtin_return_address(0));
    }
    return g_rdr;
}
void rdr_present_cpu(const uint32_t *argb, int w, int h, int pitch) {
    rdr_get()->present_argb(argb, w, h, pitch);
}
HRESULT d3d_create(ddraw_t *dd, void **out) {
    if (dd->d3d) { ((d3d7_t *)dd->d3d)->refs++; *out = dd->d3d; return 0; }
    d3d7_t *d = calloc(1, sizeof *d);
    d->vtbl = g_d3d7_vtbl; d->refs = 1; d->dd = dd;
    dd->d3d = d;
    *out = d;
    return 0;
}

void d3d_register(void) {
    static void *d3[] = { d3d_QueryInterface, d3d_AddRef, d3d_Release, d3d_EnumDevices, d3d_CreateDevice, d3d_CreateVertexBuffer,
                          d3d_EnumZBufferFormats, d3d_EvictManagedTextures };
    g_d3d7_vtbl = d3;
    static void *dv[] = { dev_QueryInterface, dev_AddRef, dev_Release, dev_GetCaps, dev_EnumTextureFormats, dev_BeginScene,
                          dev_EndScene, dev_GetDirect3D, dev_SetRenderTarget, dev_GetRenderTarget, dev_Clear, dev_SetTransform,
                          dev_GetTransform, dev_SetViewport, dev_MultiplyTransform, dev_GetViewport, dev_SetMaterial,
                          dev_GetMaterial, dev_SetLight, dev_GetLight, dev_SetRenderState, dev_GetRenderState,
                          dev_BeginStateBlock, dev_EndStateBlock, dev_PreLoad, dev_DrawPrimitive, dev_DrawIndexedPrimitive,
                          dev_SetClipStatus, dev_GetClipStatus, dev_DrawPrimitiveStrided, dev_DrawIndexedPrimitiveStrided,
                          dev_DrawPrimitiveVB, dev_DrawIndexedPrimitiveVB, dev_ComputeSphereVisibility, dev_GetTexture,
                          dev_SetTexture, dev_GetTextureStageState, dev_SetTextureStageState, dev_ValidateDevice,
                          dev_ApplyStateBlock, dev_CaptureStateBlock, dev_DeleteStateBlock, dev_CreateStateBlock, dev_Load,
                          dev_LightEnable, dev_GetLightEnable, dev_SetClipPlane, dev_GetClipPlane, dev_GetInfo };
    g_dev_vtbl = dv;
    static void *vb[] = { vb_QueryInterface, vb_AddRef, vb_Release, vb_Lock, vb_Unlock, vb_ProcessVertices,
                          vb_GetVertexBufferDesc, vb_Optimize, vb_ProcessVerticesStrided };
    g_vb_vtbl = vb;
}
