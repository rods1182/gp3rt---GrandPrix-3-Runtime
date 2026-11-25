/* GP3RT icon: embedded PNGs (art/gp3rt-*.png, generated into icons_data.h by build_linux.sh)
   installed into the user's icon theme for the menu/desktop entries, plus the window icon. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <SDL.h>
#include "icons_data.h"

/* ~/.local/share/icons/hicolor/<n>x<n>/apps/gp3rt.png for every size; returns 1 on success */
int install_icons(void) {
    const char *h = getenv("HOME"); if (!h) return 0;
    char p[4096]; int ok = 1;
    static const char *parts[] = { "/.local", "/.local/share", "/.local/share/icons", "/.local/share/icons/hicolor" };
    for (int i = 0; i < 4; i++) { snprintf(p, sizeof p, "%s%s", h, parts[i]); mkdir(p, 0755); }
    for (int i = 0; i < GP3RT_NICONS; i++) {
        int n = g_gp3rt_icons[i].size;
        snprintf(p, sizeof p, "%s/.local/share/icons/hicolor/%dx%d", h, n, n); mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/.local/share/icons/hicolor/%dx%d/apps", h, n, n); mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/.local/share/icons/hicolor/%dx%d/apps/gp3rt.png", h, n, n);
        FILE *f = fopen(p, "wb"); if (!f) { ok = 0; continue; }
        fwrite(g_gp3rt_icons[i].png, 1, g_gp3rt_icons[i].len, f); fclose(f);
        snprintf(p, sizeof p, "%s/.local/share/icons/hicolor/%dx%d/apps/gpxpatch-linux.png", h, n, n);
        f = fopen(p, "wb"); if (!f) { ok = 0; continue; }
        fwrite(g_gpxp_icons[i].png, 1, g_gpxp_icons[i].len, f); fclose(f);
    }
    return ok;
}
/* the GPxPatch icon, 128x128 RGBA (the credits page) */
const unsigned char *gpxp_icon128(void) { return g_gpxp_icon128_rgba; }
void set_window_icon_gpxp(SDL_Window *w) {
    SDL_Surface *s = SDL_CreateRGBSurfaceFrom((void *)g_gpxp_icon64_rgba, 64, 64, 32, 64 * 4, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
    if (s) { SDL_SetWindowIcon(w, s); SDL_FreeSurface(s); }
}
/* the window/taskbar icon */
void set_window_icon(SDL_Window *w) {
    SDL_Surface *s = SDL_CreateRGBSurfaceFrom((void *)g_gp3rt_icon64_rgba, 64, 64, 32, 64 * 4, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
    if (s) { SDL_SetWindowIcon(w, s); SDL_FreeSurface(s); }
}

/* the GP3RT icon on "gp3" and "GPxPatch" in the game folder (file managers show a gear for
   executables): GVfs metadata, like Properties > Icon in Nemo / Nautilus / Caja. No Wine. */
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
void set_folder_icons(const char *dir) {
    const char *h = getenv("HOME"); if (!h || !dir) return;
    if (!install_icons()) return;
    char icon[4096]; snprintf(icon, sizeof icon, "file://%s/.local/share/icons/hicolor/256x256/apps/gp3rt.png", h);
    static const char *names[] = { "gp3", "GPxPatch", NULL };
    for (int i = 0; names[i]; i++) {
        char p[4096]; snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (i == 1) snprintf(icon, sizeof icon, "file://%s/.local/share/icons/hicolor/256x256/apps/gpxpatch-linux.png", h);
        if (access(p, F_OK)) continue;
        pid_t c = fork();
        if (c == 0) { int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
            execlp("gio", "gio", "set", "-n", p, "metadata::custom-icon", icon, (char *)NULL); _exit(127); }
        if (c > 0) waitpid(c, NULL, 0);
    }
}
