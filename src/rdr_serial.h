/* portable (32/64-bit) serialisation of rstate_t for capture/replay */
#ifndef RDR_SERIAL_H
#define RDR_SERIAL_H
#include "rdr.h"
#include <stdio.h>
#define RS_FIELDS(X) X(zenable) X(zwrite) X(zfunc) X(cull) X(blend) X(srcblend) X(dstblend) X(atest) X(afunc) X(aref) \
    X(fog) X(fogtable) X(fogcolor) X(flat) X(specular) X(ckey) X(dither) X(tfactor) X(nstages) X(vx) X(vy) X(vw) X(vh)
#define RS_FFIELDS(X) X(fogstart) X(fogend) X(fogdensity) X(zbias) X(minz) X(maxz)
#define ST_FIELDS(X) X(has_alpha) X(has_ckey) X(colorop) X(carg1) X(carg2) X(alphaop) X(aarg1) X(aarg2) X(texcoord) \
    X(addru) X(addrv) X(magf) X(minf) X(mipf) X(border)
static inline void rstate_write(FILE *f, const rstate_t *s) {
    uint32_t v; float x;
#define X(n) v = (uint32_t)s->n; fwrite(&v, 4, 1, f);
    RS_FIELDS(X)
#undef X
#define X(n) x = s->n; fwrite(&x, 4, 1, f);
    RS_FFIELDS(X)
#undef X
    for (int i = 0; i < 2; i++) {
        v = (uint32_t)(uintptr_t)s->st[i].tex; fwrite(&v, 4, 1, f);
#define X(n) v = (uint32_t)s->st[i].n; fwrite(&v, 4, 1, f);
        ST_FIELDS(X)
#undef X
        x = s->st[i].lodbias; fwrite(&x, 4, 1, f);
    }
}
static inline int rstate_read(FILE *f, rstate_t *s, rtex_t *(*lookup)(uint32_t id)) {
    uint32_t v; float x; int ok = 1;
    memset(s, 0, sizeof *s);
#define X(n) ok &= fread(&v, 4, 1, f) == 1; s->n = v;
    RS_FIELDS(X)
#undef X
#define X(n) ok &= fread(&x, 4, 1, f) == 1; s->n = x;
    RS_FFIELDS(X)
#undef X
    for (int i = 0; i < 2; i++) {
        ok &= fread(&v, 4, 1, f) == 1; s->st[i].tex = v ? lookup(v) : NULL;
#define X(n) ok &= fread(&v, 4, 1, f) == 1; s->st[i].n = v;
        ST_FIELDS(X)
#undef X
        ok &= fread(&x, 4, 1, f) == 1; s->st[i].lodbias = x;
    }
    return ok;
}
#endif
