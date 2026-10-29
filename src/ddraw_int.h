/* Internal DirectDraw / Direct3D 7 structures shared between ddraw.c and d3d.c */
#ifndef DDRAW_INT_H
#define DDRAW_INT_H
#include "rt.h"
#include "osdep.h"

#pragma pack(push, 1)
typedef struct { DWORD size, flags, fourcc, bpp, rmask, gmask, bmask, amask; } DDPIXELFORMAT;
typedef struct { DWORD lo, hi; } DDCOLORKEY;
typedef struct { DWORD caps, caps2, caps3, caps4; } DDSCAPS2;
typedef struct {
    DWORD size, flags, height, width;
    LONG pitch;
    DWORD backbuffers;     /* or depth */
    DWORD mipmaps;         /* or refresh rate */
    DWORD alpha_depth, reserved;
    uint32_t surface;
    DDCOLORKEY ck_dest_overlay, ck_dest_blt, ck_src_overlay, ck_src_blt;
    DDPIXELFORMAT pf;
    DDSCAPS2 caps;
    DWORD texture_stage;
} DDSURFACEDESC2;
typedef struct { LONG left, top, right, bottom; } RECT;
#pragma pack(pop)

#define DD_OK 0
#define DDERR_GENERIC        0x80004005
#define DDERR_UNSUPPORTED    0x80004001
#define DDERR_INVALIDPARAMS  0x80070057
#define DDERR_OUTOFMEMORY    0x8007000E
#define E_NOINTERFACE        0x80004002
#define DDERR_NOTFOUND       0x887600FF
#define DDERR_SURFACELOST    0x887601C2
#define DDERR_NOTLOCKED      0x88760248
#define DDERR_NOPALETTEATTACHED 0x8876023C
#define DDERR_NOCOLORKEY     0x887600D7
#define DDERR_NOTPALETTIZED  0x88760235
#define DDERR_NOCLIPPERATTACHED 0x88760231

/* DDSD flags */
#define DDSD_CAPS 1
#define DDSD_HEIGHT 2
#define DDSD_WIDTH 4
#define DDSD_PITCH 8
#define DDSD_BACKBUFFERCOUNT 0x20
#define DDSD_ZBUFFERBITDEPTH 0x40
#define DDSD_LPSURFACE 0x800
#define DDSD_PIXELFORMAT 0x1000
#define DDSD_CKDESTBLT 0x4000
#define DDSD_CKSRCBLT 0x10000
#define DDSD_MIPMAPCOUNT 0x20000
#define DDSD_REFRESHRATE 0x40000
/* caps */
#define DDSCAPS_ALPHA 2
#define DDSCAPS_BACKBUFFER 4
#define DDSCAPS_COMPLEX 8
#define DDSCAPS_FLIP 0x10
#define DDSCAPS_FRONTBUFFER 0x20
#define DDSCAPS_OFFSCREENPLAIN 0x40
#define DDSCAPS_PALETTE 0x100
#define DDSCAPS_PRIMARYSURFACE 0x200
#define DDSCAPS_SYSTEMMEMORY 0x800
#define DDSCAPS_TEXTURE 0x1000
#define DDSCAPS_3DDEVICE 0x2000
#define DDSCAPS_VIDEOMEMORY 0x4000
#define DDSCAPS_VISIBLE 0x8000
#define DDSCAPS_ZBUFFER 0x20000
#define DDSCAPS_MIPMAP 0x400000
#define DDSCAPS_LOCALVIDMEM 0x10000000
/* pixel format flags */
#define DDPF_ALPHAPIXELS 1
#define DDPF_FOURCC 4
#define DDPF_PALETTEINDEXED8 0x20
#define DDPF_RGB 0x40
#define DDPF_ZBUFFER 0x400

typedef struct ddpal {
    void **vtbl; LONG refs;
    uint32_t xrgb[256];       /* 0x00RRGGBB */
    uint8_t raw[256][4];      /* PALETTEENTRY as given */
    DWORD caps;
    int version;
} ddpal_t;

typedef struct ddclip { void **vtbl; LONG refs; HWND hwnd; } ddclip_t;

typedef struct ddsurf ddsurf_t;
struct ddsurf {
    void **vtbl; LONG refs;
    int w, h, bpp;
    LONG pitch;
    uint8_t *bits;
    int own_bits;
    DDPIXELFORMAT pf;
    DDSCAPS2 caps;
    DWORD sd_flags;
    ddpal_t *pal;
    ddclip_t *clip;
    DDCOLORKEY ck_src, ck_dst;
    int has_ck_src, has_ck_dst;
    ddsurf_t *back;          /* flip chain: next surface */
    ddsurf_t *zbuf;          /* attached z-buffer */
    ddsurf_t *mip_next;      /* next mip level */
    ddsurf_t *chain_owner;   /* for back buffers / mips: complex root */
    int is_primary;
    int lock_count;
    int lock_ro;             /* current lock is DDLOCK_READONLY */
    uint32_t hdc;
    uint32_t version;        /* bumped on every write (texture upload tracking) */
    void *gpu;               /* renderer-side texture object */
    DWORD lod, priority;
    void *priv_data;
    void *gamma;             /* IDirectDrawGammaControl object */
    struct ddraw *dd;
};

typedef struct ddraw {
    void **vtbl; LONG refs;
    HWND hwnd; DWORD coop;
    int mode_w, mode_h, mode_bpp;
    ddsurf_t *primary;
    void *d3d;               /* IDirect3D7 object */
} ddraw_t;

extern os_mutex_t g_dd_lock;
#define DDLOCK() os_mutex_lock(&g_dd_lock)
#define DDUNLOCK() os_mutex_unlock(&g_dd_lock)

ddsurf_t *dds_create(ddraw_t *dd, const DDSURFACEDESC2 *d);
void dds_present(ddsurf_t *s);
void dds_fill_desc(ddsurf_t *s, DDSURFACEDESC2 *d);
void pf_from_bpp(DDPIXELFORMAT *pf, int bpp);
uint32_t dds_pixel_xrgb(ddsurf_t *s, int x, int y);
int guid_eq(const uint8_t *a, const char *b);

/* page-aligned surface memory (needed for fault-driven lazy readback) */
void *surf_mem_alloc(size_t n);
void surf_mem_free(void *p, size_t n);
int plat_protect(void *p, size_t n, int mode);   /* 0 none, 1 read, 2 read/write */
int d3d_guard_fault(void *addr, int is_write);   /* called from the platform fault handler */

/* d3d.c */
HRESULT d3d_create(ddraw_t *dd, void **out);
extern ddraw_t *g_ddraw;
extern ddsurf_t *g_render_target;
#endif
