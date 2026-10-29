#ifndef GP3_STDLIB_H
#define GP3_STDLIB_H
#include "_crtdefs.h"
CRTIMP void *__cdecl malloc(size_t);
CRTIMP void *__cdecl calloc(size_t, size_t);
CRTIMP void *__cdecl realloc(void *, size_t);
CRTIMP void __cdecl free(void *);
CRTIMP int __cdecl atoi(const char *);
CRTIMP double __cdecl atof(const char *);
CRTIMP unsigned long __cdecl strtoul(const char *, char **, int);
CRTIMP long __cdecl strtol(const char *, char **, int);
CRTIMP char *__cdecl getenv(const char *);
CRTIMP void __cdecl exit(int);
CRTIMP void __cdecl _exit(int);
CRTIMP int __cdecl abs(int);
#endif
