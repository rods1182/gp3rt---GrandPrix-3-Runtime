/* Finds the game executable in a folder (header-only: used by gp3, the GPxPatch screen
   and gp3-game). GP3 1.13 ships Gp3.exe; GP3 2000 ships "gp3 2000.exe". Names are
   matched without regard to case. When several are present, one without SafeDisc wins
   (SafeDisc adds stxt... sections and does not run outside Windows). */
#ifndef GAMEEXE_H
#define GAMEEXE_H
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

static const char *const GAMEEXE_NAMES[] = { "gp3 2000.exe", "gp32000.exe", "gp3_2000.exe", "gp3-2000.exe", "gp3.exe", NULL };

/* 2 = PE without SafeDisc, 1 = PE with SafeDisc, 0 = not a PE file */
static int gameexe_check(const char *path, long *size) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    unsigned char h[4096]; size_t n = fread(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END); if (size) *size = ftell(f); fclose(f);
    if (n < 0x40 || h[0] != 'M' || h[1] != 'Z') return 0;
    unsigned pe = h[0x3c] | h[0x3d] << 8 | h[0x3e] << 16 | (unsigned)h[0x3f] << 24;
    if (pe + 24 > n || memcmp(h + pe, "PE\0\0", 4)) return 0;
    unsigned ns = h[pe + 6] | h[pe + 7] << 8, opt = h[pe + 20] | h[pe + 21] << 8;
    unsigned s = pe + 24 + opt;
    for (unsigned i = 0; i < ns && s + 40 * (i + 1) <= n; i++) {
        const char *nm = (const char *)h + s + 40 * i;
        if (!strncmp(nm, "stxt", 4) || !strncmp(nm, ".stxt", 5)) return 1;
    }
    return 2;
}
/* the file name (as found in dir) of the best game executable; returns 1 when found */
static int gameexe_find(const char *dir, char *name, size_t namesz, long *size) {
    int best = 0, bestrank = 99;
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        int rank = -1;
        for (int i = 0; GAMEEXE_NAMES[i]; i++) if (!strcasecmp(e->d_name, GAMEEXE_NAMES[i])) { rank = i; break; }
        if (rank < 0) continue;
        char p[4096]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        long sz = 0; int q = gameexe_check(p, &sz); if (!q) continue;
        if (q > best || (q == best && rank < bestrank)) { best = q; bestrank = rank; snprintf(name, namesz, "%s", e->d_name); if (size) *size = sz; }
    }
    closedir(d);
    return best > 0;
}
#endif
