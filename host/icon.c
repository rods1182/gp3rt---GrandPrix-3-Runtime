/* Extracts the largest icon from Gp3.exe as a PNG (used by the menu-shortcut
 * installer: gp3 --extract-icon <exe> <out.png>). Handles BMP-style icon
 * images with 4/8/24/32 bits per pixel and PNG-compressed icons. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int write_png(const char *path, const uint32_t *xrgb, int w, int h, int pitch_px);
int write_png_rgba(const char *path, const uint32_t *argb, int w, int h);

static uint8_t *g_f; static long g_n;
static uint32_t u32(long o) { return o + 4 <= g_n ? (uint32_t)g_f[o] | g_f[o + 1] << 8 | g_f[o + 2] << 16 | (uint32_t)g_f[o + 3] << 24 : 0; }
static uint16_t u16(long o) { return o + 2 <= g_n ? (uint16_t)(g_f[o] | g_f[o + 1] << 8) : 0; }
static long g_rsrc_off; static uint32_t g_rsrc_rva;
static struct { uint32_t va, vs, ra; } g_sec[32]; static int g_nsec;
static long rva2off(uint32_t rva) { for (int i = 0; i < g_nsec; i++) if (rva >= g_sec[i].va && rva < g_sec[i].va + g_sec[i].vs) return g_sec[i].ra + (rva - g_sec[i].va); return -1; }

/* find resource data (type, id or first if id<0) -> file offset and size */
static long find_res(uint32_t type, int id, uint32_t *size) {
    long root = g_rsrc_off;
    int n = u16(root + 12) + u16(root + 14);
    for (int i = 0; i < n; i++) {
        long e = root + 16 + i * 8;
        if (u32(e) != type) continue;
        long d1 = g_rsrc_off + (u32(e + 4) & 0x7fffffff);
        int n1 = u16(d1 + 12) + u16(d1 + 14);
        for (int j = 0; j < n1; j++) {
            long e1 = d1 + 16 + j * 8;
            if (id >= 0 && u32(e1) != (uint32_t)id) continue;
            long d2 = g_rsrc_off + (u32(e1 + 4) & 0x7fffffff);
            long e2 = d2 + 16;                          /* first language */
            long de = g_rsrc_off + (u32(e2 + 4) & 0x7fffffff);
            *size = u32(de + 4);
            return rva2off(u32(de));
        }
    }
    return -1;
}

int extract_icon(const char *exe, const char *out) {
    FILE *f = fopen(exe, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); g_n = ftell(f); fseek(f, 0, SEEK_SET);
    g_f = malloc(g_n); if (fread(g_f, 1, g_n, f) != (size_t)g_n) { fclose(f); return 0; } fclose(f);
    long pe = u32(0x3c); if (u32(pe) != 0x4550) return 0;
    int ns = u16(pe + 6), so = u16(pe + 20);
    g_rsrc_rva = u32(pe + 24 + 96 + 2 * 8);            /* data directory 2: resources (PE32) */
    for (int i = 0; i < ns && i < 32; i++) { long s = pe + 24 + so + i * 40; g_sec[i].vs = u32(s + 8); g_sec[i].va = u32(s + 12); g_sec[i].ra = u32(s + 20);
        if (g_sec[i].vs < u32(s + 16)) g_sec[i].vs = u32(s + 16); g_nsec++; }
    g_rsrc_off = rva2off(g_rsrc_rva); if (g_rsrc_off < 0) return 0;
    uint32_t gsz; long g = find_res(14 /*RT_GROUP_ICON*/, -1, &gsz); if (g < 0) return 0;
    int cnt = u16(g + 4), best = -1, bestscore = -1;
    for (int i = 0; i < cnt; i++) {
        long e = g + 6 + i * 14;
        int w = g_f[e] ? g_f[e] : 256, bpp = u16(e + 6);
        int score = w * 64 + bpp;
        if (score > bestscore) { bestscore = score; best = i; }
    }
    if (best < 0) return 0;
    int id = u16(g + 6 + best * 14 + 12);
    uint32_t isz; long ic = find_res(3 /*RT_ICON*/, id, &isz); if (ic < 0) return 0;
    if (!memcmp(g_f + ic, "\x89PNG", 4)) { FILE *o = fopen(out, "wb"); if (!o) return 0; fwrite(g_f + ic, 1, isz, o); fclose(o); return 1; }
    int w = (int)u32(ic + 4), h = (int)u32(ic + 8) / 2, bpp = u16(ic + 14);
    long pal = ic + u32(ic), px = pal + (bpp <= 8 ? (4L << bpp) : 0);
    int stride = ((w * bpp + 31) / 32) * 4, mstride = ((w + 31) / 32) * 4;
    long mask = px + (long)stride * h;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return 0;
    uint32_t *img = malloc(sizeof(uint32_t) * w * h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        long r = px + (long)(h - 1 - y) * stride; uint32_t c = 0, a = 255;
        if (bpp == 32) { c = u32(r + x * 4) & 0xffffff; a = g_f[r + x * 4 + 3]; }
        else if (bpp == 24) { c = g_f[r + x * 3] | g_f[r + x * 3 + 1] << 8 | (uint32_t)g_f[r + x * 3 + 2] << 16; }
        else { int idx = bpp == 8 ? g_f[r + x] : bpp == 4 ? (g_f[r + x / 2] >> (x & 1 ? 0 : 4)) & 15 : (g_f[r + x / 8] >> (7 - (x & 7))) & 1;
               c = u32(pal + idx * 4) & 0xffffff; }
        if (bpp != 32) { long mr = mask + (long)(h - 1 - y) * mstride; if ((g_f[mr + x / 8] >> (7 - (x & 7))) & 1) a = 0; }
        img[y * w + x] = a << 24 | c;
    }
    int ok = write_png_rgba(out, img, w, h);
    free(img); free(g_f);
    return ok;
}
