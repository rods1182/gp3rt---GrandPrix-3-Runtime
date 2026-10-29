/* Minimal C library declarations for the Windows build (functions come from msvcrt.dll). */
#ifndef GP3_CRTDEFS_H
#define GP3_CRTDEFS_H
#include <stddef.h>
#include <stdarg.h>
#define CRTIMP __declspec(dllimport)
#endif
