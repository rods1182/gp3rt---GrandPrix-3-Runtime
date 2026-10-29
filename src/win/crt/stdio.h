#ifndef GP3_STDIO_H
#define GP3_STDIO_H
#include "_crtdefs.h"
typedef struct _iobuf FILE;
CRTIMP FILE *__cdecl fopen(const char *, const char *);
CRTIMP int __cdecl fclose(FILE *);
CRTIMP size_t __cdecl fread(void *, size_t, size_t, FILE *);
CRTIMP size_t __cdecl fwrite(const void *, size_t, size_t, FILE *);
CRTIMP int __cdecl fflush(FILE *);
CRTIMP int __cdecl fputc(int, FILE *);
CRTIMP int __cdecl fputs(const char *, FILE *);
CRTIMP int __cdecl fprintf(FILE *, const char *, ...);
CRTIMP int __cdecl vfprintf(FILE *, const char *, va_list);
CRTIMP int __cdecl sprintf(char *, const char *, ...);
CRTIMP int __cdecl sscanf(const char *, const char *, ...);
CRTIMP int __cdecl fseek(FILE *, long, int);
CRTIMP long __cdecl ftell(FILE *);
CRTIMP char *__cdecl fgets(char *, int, FILE *);
/* C99 snprintf semantics on top of msvcrt's _vsnprintf (implemented in win_crt.c) */
int __cdecl gp3_vsnprintf(char *, size_t, const char *, va_list);
int __cdecl gp3_snprintf(char *, size_t, const char *, ...);
#define vsnprintf gp3_vsnprintf
#define snprintf gp3_snprintf
FILE *gp3_stderr(void);
#define stderr gp3_stderr()
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
