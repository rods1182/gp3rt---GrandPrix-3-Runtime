/* Platform backend interface: window, presentation, input pump, audio. */
#ifndef PLAT_H
#define PLAT_H
#include <stdint.h>
void plat_pump(void);                       /* process host events (called from message APIs) */
void plat_present(const uint32_t *xrgb, int w, int h, int pitch_px);  /* show a frame (0x00RRGGBB) */
void plat_client_size(int *w, int *h);      /* current game display size */
void plat_set_mode(int w, int h, int bpp);  /* game changed display mode */
void plat_show_cursor(int show);
void plat_message_box(const char *cap, const char *text);
void plat_screenshot(const char *name);     /* request a dump of the next presented frame */
void plat_set_window(void *hwnd, unsigned coop_flags);   /* SetCooperativeLevel */
int write_png(const char *path, const uint32_t *xrgb, int w, int h, int pitch_px);
#endif
