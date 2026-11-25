/* GPxPatch icon for the window (the .exe icon itself is in the resources) */
#include <SDL.h>
#include "../icons_data.h"
const unsigned char *gpxp_icon128(void) { return g_gpxp_icon128_rgba; }
void set_window_icon_gpxp(SDL_Window *w) {
    SDL_Surface *s = SDL_CreateRGBSurfaceFrom((void *)g_gpxp_icon64_rgba, 64, 64, 32, 64 * 4, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
    if (s) { SDL_SetWindowIcon(w, s); SDL_FreeSurface(s); }
}
