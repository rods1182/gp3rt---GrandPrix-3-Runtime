/* Renderer backend interface.
 * Direct3D 7 (d3d.c) translates everything the game does into these calls.
 * Backends: rdr_soft.c (software rasteriser: tests / fallback),
 *           rdr_gl.c   (OpenGL 3.3: Windows and Linux host). */
#ifndef RDR_H
#define RDR_H
#include <stdint.h>

/* vertex after FVF decoding; position is in clip space (x,y,z,w) */
typedef struct {
    float x, y, z, w;
    uint32_t diffuse, specular;   /* 0xAARRGGBB */
    float uv[2][2];
} rvert_t;

/* texture stage operations (values match D3DTEXTUREOP) */
enum { TOP_DISABLE = 1, TOP_SELECTARG1, TOP_SELECTARG2, TOP_MODULATE, TOP_MODULATE2X, TOP_MODULATE4X, TOP_ADD,
       TOP_ADDSIGNED, TOP_ADDSIGNED2X, TOP_SUBTRACT, TOP_ADDSMOOTH, TOP_BLENDDIFFUSEALPHA, TOP_BLENDTEXTUREALPHA,
       TOP_BLENDFACTORALPHA, TOP_BLENDTEXTUREALPHAPM, TOP_BLENDCURRENTALPHA, TOP_PREMODULATE,
       TOP_MODULATEALPHA_ADDCOLOR, TOP_MODULATECOLOR_ADDALPHA, TOP_MODULATEINVALPHA_ADDCOLOR,
       TOP_MODULATEINVCOLOR_ADDALPHA, TOP_BUMPENVMAP, TOP_BUMPENVMAPLUMINANCE, TOP_DOTPRODUCT3 };
/* texture args (D3DTA_*) */
enum { TA_DIFFUSE = 0, TA_CURRENT = 1, TA_TEXTURE = 2, TA_TFACTOR = 3, TA_SPECULAR = 4, TA_COMPLEMENT = 0x10, TA_ALPHAREPLICATE = 0x20 };

typedef struct rtex rtex_t;        /* backend texture */
typedef struct rtarget rtarget_t;  /* backend render target (colour + depth) */

typedef struct {
    rtex_t *tex;
    int has_alpha, has_ckey;
    int colorop, carg1, carg2, alphaop, aarg1, aarg2;
    int texcoord;                  /* TEXCOORDINDEX */
    int addru, addrv;              /* 1 wrap, 2 mirror, 3 clamp, 4 border */
    int magf, minf, mipf;          /* 1 point, 2 linear (mipf: 1 none, 2 point, 3 linear) */
    uint32_t border;
    float lodbias;
} rstage_t;

typedef struct {
    int zenable, zwrite, zfunc;    /* zfunc: D3DCMP 1..8 */
    int cull;                      /* 1 none, 2 cw, 3 ccw */
    int blend, srcblend, dstblend; /* D3DBLEND values */
    int atest, afunc; uint32_t aref;
    int fog; int fogtable;         /* fogtable: 0 none (vertex fog), 1 exp, 2 exp2, 3 linear */
    uint32_t fogcolor; float fogstart, fogend, fogdensity;
    int flat, specular, ckey, dither;
    uint32_t tfactor;
    float zbias;
    int nstages;
    rstage_t st[2];
    /* viewport / scissor in target pixels */
    int vx, vy, vw, vh; float minz, maxz;
} rstate_t;

enum { RPRIM_POINTS = 1, RPRIM_LINES = 2, RPRIM_LINESTRIP = 3, RPRIM_TRIS = 4, RPRIM_TRISTRIP = 5, RPRIM_TRIFAN = 6 };

typedef struct rbackend {
    const char *name;
    rtex_t *(*tex_create)(int w, int h, int levels);
    void (*tex_upload)(rtex_t *t, int level, int w, int h, const uint32_t *argb);
    void (*tex_destroy)(rtex_t *t);
    rtarget_t *(*target_create)(int w, int h, int scale);
    void (*target_destroy)(rtarget_t *t);
    /* CPU <-> GPU copies of the colour buffer (argb, game resolution) */
    void (*target_upload)(rtarget_t *t, const uint32_t *argb, int pitch_px, int x, int y, int w, int h);
    /* rows y0..y0+n-1 of the colour buffer into argb (argb points at row 0) */
    void (*target_download)(rtarget_t *t, uint32_t *argb, int pitch_px, int y0, int n);
    void (*clear)(rtarget_t *t, const int *rects, int nrects, int color, uint32_t argb, int depth, float z, int vx, int vy, int vw, int vh);
    void (*draw)(rtarget_t *t, const rstate_t *s, int prim, const rvert_t *v, int nv, const uint16_t *idx, int ni);
    /* textured quad from argb pixels into the target (DirectDraw Blt onto a 3D surface) */
    void (*blit)(rtarget_t *t, int dx, int dy, int dw, int dh, const uint32_t *argb, int sw, int sh, int pitch_px, int keyed);
    /* copy a rectangle inside the same target (stretching allowed) */
    void (*copy)(rtarget_t *t, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh);
    void (*present)(rtarget_t *t, int w, int h);
    void (*present_argb)(const uint32_t *argb, int w, int h, int pitch_px);
    /* copy a rectangle from another target (stretching allowed) */
    void (*copy_from)(rtarget_t *dst, int dx, int dy, int dw, int dh, rtarget_t *src, int sx, int sy, int sw, int sh);
} rbackend_t;

extern rbackend_t *g_rdr;
rbackend_t *rdr_soft_backend(void);
#endif
