#ifndef GAMEPATCH_H
#define GAMEPATCH_H
#include <stdint.h>
/* addresses that differ between GP3 builds */
typedef struct {
    const char *name;
    uint32_t fov_site; uint8_t fov_orig[6];   /* "mov [xscale], ecx" replaced by the Hor+ stub */
    uint32_t xscale, height;                  /* focal-length scale, current screen height */
    uint32_t res_table;                       /* 30 entries x 36 bytes */
    uint32_t ovl_sites[2];                    /* Lock callers that only write the back buffer */
    int32_t shadow_delta;                     /* offset of the shadow-read routine vs 1.13 */
} gp3_addrs_t;
const gp3_addrs_t *gp3_addrs(void);
void gp3_addrs_reset(void);       /* NULL = unknown build */
void gp3_widen_modes(int w, int h);   /* menu 1280x1024 entries -> w x h */
void gp3_hor_plus(int fov_percent);
void gp3_fix_string_table(void);   /* GP3 1.13: short ldata\\*.str files */   /* Hor+ field of view for wide screens */
int plat_patch_code(void *addr, const void *bytes, int n);
int plat_make_writable(void *addr, int n);
#endif
