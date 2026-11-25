#ifndef TTF_H
#define TTF_H
#include <stdint.h>
#include <stddef.h>
typedef struct ttf {
    uint8_t *data; size_t size;
    uint32_t loca, glyf, hmtx, cmap_sub;
    int cmap_fmt, symbol, loca_long, upem, nhmetrics, nglyphs, bold, italic;
    int ascent, descent, linegap;
    uint16_t win_ascent, win_descent;
    char family[64];
} ttf_t;
ttf_t *ttf_load(const char *path);
int ttf_glyph_index(const ttf_t *t, uint32_t cp);
int ttf_advance(const ttf_t *t, int g);
uint8_t *ttf_render(const ttf_t *t, int g, float px, float shear, float embolden, int *w, int *h, int *ox, int *oy);
#endif
