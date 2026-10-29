/* Minimal PNG writer (stored deflate blocks, no compression library). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* ------------------------------------------------------------------ */
/* PNG (stored deflate blocks, no compression library needed)           */
static uint32_t crc_tab[256];
static uint32_t crc32_(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xedb88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
    c = ~c; while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8); return ~c;
}
static void put32(FILE *f, uint32_t v) { uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v }; fwrite(b, 1, 4, f); }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    put32(f, n); fwrite(t, 1, 4, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32_(0, (const uint8_t *)t, 4); c = crc32_(c, d, n); put32(f, c);
}
int write_png(const char *path, const uint32_t *xrgb, int w, int h, int pitch_px) {
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0 };
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (1 + w * 3);
    uint8_t *raw = malloc(raw_n), *p = raw;
    for (int y = 0; y < h; y++) {
        *p++ = 0;
        for (int x = 0; x < w; x++) { uint32_t c = xrgb[(size_t)y * pitch_px + x]; *p++ = c >> 16; *p++ = c >> 8; *p++ = c; }
    }
    size_t nblk = (raw_n + 65534) / 65535;
    size_t zn = 2 + raw_n + nblk * 5 + 4;
    uint8_t *z = malloc(zn), *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        *q++ = off + n >= raw_n; *q++ = n; *q++ = n >> 8; *q++ = ~n; *q++ = (~n) >> 8;
        memcpy(q, raw + off, n); q += n;
    }
    uint32_t ad = (b << 16) | a; *q++ = ad >> 24; *q++ = ad >> 16; *q++ = ad >> 8; *q++ = ad;
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
    return 1;
}

/* RGBA variant (icons) */
int write_png_rgba(const char *path, const uint32_t *argb, int w, int h) {
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 6, 0, 0, 0 };
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (1 + w * 4);
    uint8_t *raw = malloc(raw_n), *p = raw;
    for (int y = 0; y < h; y++) {
        *p++ = 0;
        for (int x = 0; x < w; x++) { uint32_t c = argb[(size_t)y * w + x]; *p++ = c >> 16; *p++ = c >> 8; *p++ = c; *p++ = c >> 24; }
    }
    size_t nblk = (raw_n + 65534) / 65535, zn = 2 + raw_n + nblk * 5 + 4;
    uint8_t *z = malloc(zn), *q = z; *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        *q++ = off + n >= raw_n; *q++ = n; *q++ = n >> 8; *q++ = ~n; *q++ = (~n) >> 8;
        memcpy(q, raw + off, n); q += n;
    }
    uint32_t ad = (b << 16) | a; *q++ = ad >> 24; *q++ = ad >> 16; *q++ = ad >> 8; *q++ = ad;
    chunk(f, "IDAT", z, (uint32_t)(q - z)); chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
    return 1;
}
