/* Nuklear implementation + an SDL_Renderer backend (adapted from Nuklear's demo/sdl_renderer).
   SDL_RenderGeometryRaw only exists since SDL 2.0.18: it is looked up at run time so that gp3
   still starts on older systems (the GPxPatch screen then falls back to the browser). */
/* keep gp3 runnable on glibc 2.17: bind the old versions of the libm functions Nuklear uses */
#ifndef _WIN32
__asm__(".symver pow,pow@GLIBC_2.2.5");
__asm__(".symver fmod,fmod@GLIBC_2.2.5");
#endif
#define NK_IMPLEMENTATION
#include "nk_conf.h"
#include <string.h>
#include <stdlib.h>

typedef int (*geom_raw_t)(SDL_Renderer *, SDL_Texture *, const float *, int, const SDL_Color *, int,
                          const float *, int, int, const void *, int, int);
static geom_raw_t p_geom;

static struct {
    SDL_Window *win; SDL_Renderer *renderer;
    struct nk_buffer cmds; struct nk_draw_null_texture tex_null; SDL_Texture *font_tex;
    struct nk_context ctx; struct nk_font_atlas atlas; Uint32 last;
} sdl;

struct vtx { float pos[2]; float uv[2]; nk_byte col[4]; };

int nk_sdl_available(void) {
#ifdef _WIN32
    p_geom = SDL_RenderGeometryRaw;   /* Windows: SDL 2.30 linked in */
#endif
    if (!p_geom) {   /* through SDL itself (no dlsym: that needs glibc 2.34) */
        static const char *names[] = { "libSDL2-2.0.so.0", "libSDL2.so", NULL };
        for (int i = 0; names[i] && !p_geom; i++) { void *h = SDL_LoadObject(names[i]); if (h) p_geom = (geom_raw_t)SDL_LoadFunction(h, "SDL_RenderGeometryRaw"); }
    }
    return p_geom != NULL;
}
static void clip_paste(nk_handle u, struct nk_text_edit *e) { (void)u; char *t = SDL_GetClipboardText(); if (t) { nk_textedit_paste(e, t, nk_strlen(t)); SDL_free(t); } }
static void clip_copy(nk_handle u, const char *t, int n) { (void)u; if (n <= 0) return; char *s = malloc((size_t)n + 1); if (!s) return; memcpy(s, t, (size_t)n); s[n] = 0; SDL_SetClipboardText(s); free(s); }

struct nk_context *nk_sdl_init(SDL_Window *win, SDL_Renderer *r) {
    if (!nk_sdl_available()) return NULL;
    sdl.win = win; sdl.renderer = r; sdl.last = SDL_GetTicks();
    nk_init_default(&sdl.ctx, 0);
    sdl.ctx.clip.copy = clip_copy; sdl.ctx.clip.paste = clip_paste; sdl.ctx.clip.userdata = nk_handle_ptr(0);
    nk_buffer_init_default(&sdl.cmds);
    return &sdl.ctx;
}
void nk_sdl_font_stash_begin(void) { nk_font_atlas_init_default(&sdl.atlas); nk_font_atlas_begin(&sdl.atlas); }
struct nk_font *nk_sdl_add_font(const void *ttf, int size, float px) {
    struct nk_font_config cfg = nk_font_config(px);
    static const nk_rune ranges[] = { 0x0020, 0x00FF, 0x0152, 0x0153, 0x2013, 0x2026, 0x2190, 0x2193, 0x2212, 0x2212, 0x25B6, 0x25BC, 0x26A0, 0x26A0, 0x2713, 0x2713, 0 };
    cfg.range = ranges; cfg.oversample_h = 2; cfg.oversample_v = 1;
    return nk_font_atlas_add_from_memory(&sdl.atlas, (void *)ttf, (nk_size)size, px, &cfg);
}
/* a font with its own glyph ranges (the classic GPxPatch screen: Latin, Latin-1/A, Cyrillic) */
struct nk_font *nk_sdl_add_font_ranges(const void *ttf, int size, float px, const nk_rune *ranges) {
    struct nk_font_config cfg = nk_font_config(px);
    cfg.range = ranges; cfg.oversample_h = 3; cfg.oversample_v = 1; cfg.pixel_snap = 1;
    return nk_font_atlas_add_from_memory(&sdl.atlas, (void *)ttf, (nk_size)size, px, &cfg);
}
SDL_Texture *nk_sdl_font_texture(void) { return sdl.font_tex; }
void nk_sdl_font_stash_end(void) {
    int w, h; const void *img = nk_font_atlas_bake(&sdl.atlas, &w, &h, NK_FONT_ATLAS_RGBA32);
    SDL_Texture *t = SDL_CreateTexture(sdl.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) { SDL_UpdateTexture(t, NULL, img, 4 * w); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); }
    sdl.font_tex = t;
    nk_font_atlas_end(&sdl.atlas, nk_handle_ptr(t), &sdl.tex_null);
    if (sdl.atlas.default_font) nk_style_set_font(&sdl.ctx, &sdl.atlas.default_font->handle);
}

void nk_sdl_render(void) {
    static const struct nk_draw_vertex_layout_element layout[] = {
        { NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(struct vtx, pos) },
        { NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(struct vtx, uv) },
        { NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, NK_OFFSETOF(struct vtx, col) },
        { NK_VERTEX_LAYOUT_END } };
    Uint32 now = SDL_GetTicks(); sdl.ctx.delta_time_seconds = (float)(now - sdl.last) / 1000.f; sdl.last = now;
    struct nk_convert_config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.vertex_layout = layout; cfg.vertex_size = sizeof(struct vtx); cfg.vertex_alignment = NK_ALIGNOF(struct vtx);
    cfg.tex_null = sdl.tex_null; cfg.circle_segment_count = 22; cfg.curve_segment_count = 22; cfg.arc_segment_count = 22;
    cfg.global_alpha = 1.0f; cfg.shape_AA = NK_ANTI_ALIASING_ON; cfg.line_AA = NK_ANTI_ALIASING_ON;
    struct nk_buffer vbuf, ebuf; nk_buffer_init_default(&vbuf); nk_buffer_init_default(&ebuf);
    nk_convert(&sdl.ctx, &sdl.cmds, &vbuf, &ebuf, &cfg);
    const nk_draw_index *off = (const nk_draw_index *)nk_buffer_memory_const(&ebuf);
    const struct nk_draw_command *cmd;
    int vs = sizeof(struct vtx);
    const nk_byte *v = (const nk_byte *)nk_buffer_memory_const(&vbuf);
    SDL_Rect vp; SDL_RenderGetViewport(sdl.renderer, &vp);
    nk_draw_foreach(cmd, &sdl.ctx, &sdl.cmds) {
        if (!cmd->elem_count) continue;
        SDL_Rect r = { (int)cmd->clip_rect.x, (int)cmd->clip_rect.y, (int)cmd->clip_rect.w, (int)cmd->clip_rect.h };
        if (r.x < 0) { r.w += r.x; r.x = 0; } if (r.y < 0) { r.h += r.y; r.y = 0; }
        if (r.w > vp.w) r.w = vp.w; if (r.h > vp.h) r.h = vp.h;
        if (r.w <= 0 || r.h <= 0) { off += cmd->elem_count; continue; }
        SDL_RenderSetClipRect(sdl.renderer, &r);
        p_geom(sdl.renderer, (SDL_Texture *)cmd->texture.ptr,
               (const float *)(v + NK_OFFSETOF(struct vtx, pos)), vs,
               (const SDL_Color *)(v + NK_OFFSETOF(struct vtx, col)), vs,
               (const float *)(v + NK_OFFSETOF(struct vtx, uv)), vs,
               (int)(vbuf.needed / (size_t)vs), off, (int)cmd->elem_count, 4);
        off += cmd->elem_count;
    }
    SDL_RenderSetClipRect(sdl.renderer, NULL);
    nk_clear(&sdl.ctx); nk_buffer_clear(&sdl.cmds); nk_buffer_free(&vbuf); nk_buffer_free(&ebuf);
}

int nk_sdl_handle_event(SDL_Event *e, float sc) {
    struct nk_context *ctx = &sdl.ctx;
    int ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
    switch (e->type) {
    case SDL_KEYUP: case SDL_KEYDOWN: {
        int d = e->type == SDL_KEYDOWN;
        switch (e->key.keysym.sym) {
        case SDLK_RSHIFT: case SDLK_LSHIFT: nk_input_key(ctx, NK_KEY_SHIFT, d); break;
        case SDLK_DELETE: nk_input_key(ctx, NK_KEY_DEL, d); break;
        case SDLK_KP_ENTER: case SDLK_RETURN: nk_input_key(ctx, NK_KEY_ENTER, d); break;
        case SDLK_TAB: nk_input_key(ctx, NK_KEY_TAB, d); break;
        case SDLK_BACKSPACE: nk_input_key(ctx, NK_KEY_BACKSPACE, d); break;
        case SDLK_HOME: nk_input_key(ctx, NK_KEY_TEXT_START, d); nk_input_key(ctx, NK_KEY_SCROLL_START, d); break;
        case SDLK_END: nk_input_key(ctx, NK_KEY_TEXT_END, d); nk_input_key(ctx, NK_KEY_SCROLL_END, d); break;
        case SDLK_PAGEDOWN: nk_input_key(ctx, NK_KEY_SCROLL_DOWN, d); break;
        case SDLK_PAGEUP: nk_input_key(ctx, NK_KEY_SCROLL_UP, d); break;
        case SDLK_z: nk_input_key(ctx, NK_KEY_TEXT_UNDO, d && ctrl); break;
        case SDLK_r: nk_input_key(ctx, NK_KEY_TEXT_REDO, d && ctrl); break;
        case SDLK_c: nk_input_key(ctx, NK_KEY_COPY, d && ctrl); break;
        case SDLK_v: nk_input_key(ctx, NK_KEY_PASTE, d && ctrl); break;
        case SDLK_x: nk_input_key(ctx, NK_KEY_CUT, d && ctrl); break;
        case SDLK_a: if (ctrl) nk_input_key(ctx, NK_KEY_TEXT_SELECT_ALL, d); break;
        case SDLK_UP: nk_input_key(ctx, NK_KEY_UP, d); break;
        case SDLK_DOWN: nk_input_key(ctx, NK_KEY_DOWN, d); break;
        case SDLK_LEFT: nk_input_key(ctx, ctrl ? NK_KEY_TEXT_WORD_LEFT : NK_KEY_LEFT, d); break;
        case SDLK_RIGHT: nk_input_key(ctx, ctrl ? NK_KEY_TEXT_WORD_RIGHT : NK_KEY_RIGHT, d); break;
        }
        return 1; }
    case SDL_MOUSEBUTTONUP: case SDL_MOUSEBUTTONDOWN: {
        int d = e->type == SDL_MOUSEBUTTONDOWN, x = (int)(e->button.x * sc), y = (int)(e->button.y * sc);
        if (e->button.button == SDL_BUTTON_LEFT) { if (e->button.clicks > 1) nk_input_button(ctx, NK_BUTTON_DOUBLE, x, y, d); nk_input_button(ctx, NK_BUTTON_LEFT, x, y, d); }
        else if (e->button.button == SDL_BUTTON_MIDDLE) nk_input_button(ctx, NK_BUTTON_MIDDLE, x, y, d);
        else if (e->button.button == SDL_BUTTON_RIGHT) nk_input_button(ctx, NK_BUTTON_RIGHT, x, y, d);
        return 1; }
    case SDL_MOUSEMOTION: nk_input_motion(ctx, (int)(e->motion.x * sc), (int)(e->motion.y * sc)); return 1;
    case SDL_TEXTINPUT: { nk_glyph g; memcpy(g, e->text.text, NK_UTF_SIZE); nk_input_glyph(ctx, g); return 1; }
    case SDL_MOUSEWHEEL: nk_input_scroll(ctx, nk_vec2((float)e->wheel.x, (float)e->wheel.y)); return 1;
    }
    return 0;
}
void nk_sdl_shutdown(void) {
    nk_font_atlas_clear(&sdl.atlas); nk_free(&sdl.ctx);
    if (sdl.font_tex) SDL_DestroyTexture(sdl.font_tex);
    nk_buffer_free(&sdl.cmds); memset(&sdl, 0, sizeof sdl);
}
