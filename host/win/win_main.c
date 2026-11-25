/* GPxPatch GP3RT for Windows: the GPxPatch configuration screen of GP3RT as a
   Windows program. Put "GPxPatch GP3RT.exe" in the game folder (next to the game
   executable and the gpxpdata folder), or pass the folder as the first argument. */
#define SDL_MAIN_HANDLED
#include "win_compat.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <SDL.h>
#include "../../src/gameexe.h"
#define VERSION "1.0.0"
int gpxui_main(const char *game_dir, const char *version);
static FILE *g_log;
void hlog(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    fprintf(g_log, "[%7.3f] ", (double)GetTickCount() / 1000.0); vfprintf(g_log, fmt, ap); fputc('\n', g_log); fflush(g_log);
    va_end(ap);
}
static int is_game_dir(const char *d) {
    char en[256], p[PATH_MAX]; DIR *x;
    if (gameexe_find(d, en, sizeof en, NULL)) return 1;
    snprintf(p, sizeof p, "%s/gpxpdata", d); x = opendir(p); if (x) { closedir(x); return 1; }
    return 0;
}
int main(int argc, char **argv) {
    char dir[PATH_MAX] = "";
    if (argc > 1 && argv[1][0] != '-') snprintf(dir, sizeof dir, "%s", argv[1]);
    else { char self[PATH_MAX]; GetModuleFileNameA(NULL, self, sizeof self); char *sl = strrchr(self, '\\'); if (sl) *sl = 0; snprintf(dir, sizeof dir, "%s", self); }
    win_slashes(dir); { size_t n = strlen(dir); while (n > 3 && dir[n - 1] == '/') dir[--n] = 0; }
    if (!is_game_dir(dir)) {
        MessageBoxA(NULL, "Coloque o \"GPxPatch GP3RT.exe\" na pasta do jogo (onde est\xc3\xa3o o Gp3.exe ou o \"gp3 2000.exe\" e a pasta gpxpdata).\n\n"
                          "Put \"GPxPatch GP3RT.exe\" in the game folder (where Gp3.exe or \"gp3 2000.exe\" and the gpxpdata folder are).",
                    "GPxPatch GP3RT", MB_ICONWARNING | MB_OK);
        return 1;
    }
    char lp[PATH_MAX]; snprintf(lp, sizeof lp, "%s/gpxpatch-gp3rt.log", dir); g_log = fopen(lp, "w");
    hlog("GPxPatch GP3RT for Windows %s - game folder %s", VERSION, dir);
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
    int r = gpxui_main(dir, VERSION);
    if (r < 0) MessageBoxA(NULL, "N\xc3\xa3o foi poss\xc3\xadvel abrir a janela (veja gpxpatch-gp3rt.log).\n\nCould not open the window (see gpxpatch-gp3rt.log).", "GPxPatch GP3RT", MB_ICONERROR | MB_OK);
    if (g_log) fclose(g_log);
    return r < 0 ? 1 : 0;
}
