/* GP3RT for Linux - shared by the two GPxPatch screens: the native one (host/gpxui.c)
   and the browser one (host/launcher.c, which owns these functions). */
#ifndef GP3_LAUNCHER_H
#define GP3_LAUNCHER_H
#include <stddef.h>
void lx_setup(const char *game_dir, const char *version);   /* game folder, its gpxpdata */
const char *lx_dir(void);
const char *lx_gpxdir(void);                     /* "" when GPxPatch is not installed */
const char *lx_version(void);
const char *lx_exe_version(void);                /* "1.13", "2000" or "?" */
int lx_find_ci(const char *dir, const char *name, char *out, size_t outsz);
int lx_ci_walk(const char *base, const char *rel, char *out, size_t outsz);
int lx_game_file(const char *win, char *out, size_t outsz);   /* GP3 path -> host path, 1 if it exists */
char *lx_read_file(const char *path, size_t *len);
int lx_file_path(const char *name, char *out, size_t outsz);  /* gp3rt.ini or a plain name in gpxpdata */
int lx_ini_set(const char *path, const char *sec, const char *key, const char *val);   /* val NULL = delete */
char lx_set_kind(const char *path);              /* 's' season, 'c' car set, 't' track set */
int lx_is_utf8(const char *s, size_t n);
void lx_utf8_to_latin1(char *s);
int lx_make_shortcuts(void);
int lx_shortcuts_exist(void);
void lx_folder_launchers(void);                  /* "Grand Prix 3" / "GPxPatch" launchers with our icon in the game folder */
/* GPxPatch 4.52 shipped (unmodified, with the author's permission) in "GPxPatch-4.52" next to
   the game: 1 when that folder is there; install copies it into the game folder without
   overwriting anything already there (returns files copied, -1 on error) */
int lx_bundled_gpx(void);
int lx_install_bundled_gpx(void);
void lx_start_game(int with_gpx);
int lx_game_running(void);                       /* also reaps a finished game */
int lx_last_exit(void);
#endif
