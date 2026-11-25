/* GPxPatch GP3RT for Windows: the few POSIX pieces the GPxPatch screen uses.
   Paths inside the program use '/' (Windows accepts both); file names are UTF-8
   (the manifest sets the process code page to UTF-8 on Windows 10 1903+). */
#ifndef WIN_COMPAT_H
#define WIN_COMPAT_H
#include <stdio.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#undef PATH_MAX
#define PATH_MAX 1024
static inline void win_slashes(char *s) { for (; *s; s++) if (*s == '\\') *s = '/'; }
/* absolute path of an existing file or folder, with '/' and no trailing '/' (except "C:/") */
static inline char *win_realpath(const char *p, char *out) {
    char t[PATH_MAX]; size_t n;
    if (!p || !*p) return NULL;
    if (strlen(p) == 2 && p[1] == ':') { snprintf(t, sizeof t, "%s/", p); p = t; }
    char full[PATH_MAX]; if (!_fullpath(full, p, sizeof full)) return NULL;
    DWORD a = GetFileAttributesA(full); if (a == INVALID_FILE_ATTRIBUTES) return NULL;
    win_slashes(full); n = strlen(full);
    while (n > 3 && full[n - 1] == '/') full[--n] = 0;
    strcpy(out, full); return out;
}
#define realpath(p, o) win_realpath(p, o)
#endif
