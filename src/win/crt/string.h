#ifndef GP3_STRING_H
#define GP3_STRING_H
#include "_crtdefs.h"
CRTIMP void *__cdecl memcpy(void *, const void *, size_t);
CRTIMP void *__cdecl memmove(void *, const void *, size_t);
CRTIMP void *__cdecl memset(void *, int, size_t);
CRTIMP int __cdecl memcmp(const void *, const void *, size_t);
CRTIMP size_t __cdecl strlen(const char *);
CRTIMP int __cdecl strcmp(const char *, const char *);
CRTIMP int __cdecl strncmp(const char *, const char *, size_t);
CRTIMP char *__cdecl strcpy(char *, const char *);
CRTIMP char *__cdecl strncpy(char *, const char *, size_t);
CRTIMP char *__cdecl strcat(char *, const char *);
CRTIMP char *__cdecl strchr(const char *, int);
CRTIMP char *__cdecl strrchr(const char *, int);
CRTIMP char *__cdecl strstr(const char *, const char *);
CRTIMP int __cdecl _stricmp(const char *, const char *);
CRTIMP int __cdecl _strnicmp(const char *, const char *, size_t);
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif
