/* Nuklear configuration shared by every file that uses it (must be identical everywhere) */
#ifndef GP3_NK_CONF_H
#define GP3_NK_CONF_H
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_UINT_DRAW_INDEX
#include "third_party/nuklear.h"
#include <SDL.h>
/* host/nk_sdl.c: Nuklear on an SDL_Renderer (SDL_RenderGeometryRaw, looked up at run time) */
int nk_sdl_available(void);                          /* 0: the system SDL is older than 2.0.18 */
struct nk_context *nk_sdl_init(SDL_Window *win, SDL_Renderer *r);
struct nk_font *nk_sdl_add_font(const void *ttf, int size, float px);   /* call between stash begin/end */
void nk_sdl_font_stash_begin(void);
void nk_sdl_font_stash_end(void);
struct nk_font *nk_sdl_add_font_ranges(const void *ttf, int size, float px, const nk_rune *ranges);
SDL_Texture *nk_sdl_font_texture(void);
int nk_sdl_handle_event(SDL_Event *e, float scale);  /* scale: drawable pixels per window point */
void nk_sdl_render(void);
void nk_sdl_shutdown(void);
#endif
