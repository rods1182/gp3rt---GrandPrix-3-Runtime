#ifndef GP3_MATH_H
#define GP3_MATH_H
#include "_crtdefs.h"
CRTIMP double __cdecl floor(double);
CRTIMP double __cdecl ceil(double);
CRTIMP double __cdecl exp(double);
CRTIMP double __cdecl log(double);
CRTIMP double __cdecl pow(double, double);
CRTIMP double __cdecl sqrt(double);
CRTIMP double __cdecl fabs(double);
static inline float floorf(float x) { return (float)floor(x); }
static inline float ceilf(float x) { return (float)ceil(x); }
static inline float expf(float x) { return (float)exp(x); }
static inline float log2f(float x) { return (float)(log(x) * 1.4426950408889634); }
static inline float sqrtf(float x) { return (float)sqrt(x); }
static inline float fabsf(float x) { return x < 0 ? -x : x; }
static inline float fminf(float a, float b) { return a < b ? a : b; }
static inline float fmaxf(float a, float b) { return a > b ? a : b; }
#define isfinite(x) __builtin_isfinite(x)
#endif
