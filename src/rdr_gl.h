#ifndef RDR_GL_H
#define RDR_GL_H
#include "rdr.h"
typedef struct {
    void *ctx;
    void *(*getproc)(const char *name);
    void (*swap)(void *ctx);
    void (*window_size)(void *ctx, int *w, int *h);
    void (*log)(const char *msg);
    unsigned (*window_fbo)(void *ctx);   /* optional: framebuffer standing in for the window (tests) */
} rdr_gl_host_t;
typedef struct {
    int scale;            /* internal render scale (1 = game resolution) */
    int anisotropy;       /* 0/1 = off */
    int force_linear;     /* force bilinear + trilinear filtering */
    int generate_mipmaps; /* build mip chains for textures without one */
} rdr_gl_options_t;
rbackend_t *rdr_gl_init(const rdr_gl_host_t *host, const rdr_gl_options_t *opt);
#endif
