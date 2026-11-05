#ifndef GDI_H
#define GDI_H
#include "rt.h"
typedef struct {
    void *bits; int w, h, pitch, bpp;
    uint32_t pal[256];       /* palette entries as 0x00RRGGBB */
    uint32_t masks[3];
} gdi_view_t;
typedef struct {
    int w, h, bpp, pitch, bottom_up, ncolors;
    uint8_t *bits;
    uint32_t pal[256];
    uint32_t masks[3];
} gdi_bitmap_t;
typedef struct {
    int is_surface, is_window;
    gdi_view_t surf;
    uint32_t bitmap, font;
    uint32_t textcolor, bkcolor;   /* COLORREF 0x00BBGGRR */
    int bkmode, align;             /* bkmode 1 = TRANSPARENT, 2 = OPAQUE */
} gdi_dc_t;
typedef struct {
    int height, width, weight, italic, fixed;
    char face[64];
    struct ttf *ttf; int resolved;
} gdi_font_t;
uint32_t gdi_surface_dc(void *bits, int w, int h, int pitch, int bpp, const uint32_t *pal, const uint32_t masks[3]);
void gdi_free_dc(uint32_t h);
gdi_dc_t *gdi_get_dc(uint32_t h);
#endif
