/* Small C-runtime glue for the Windows build (msvcrt.dll provides the rest). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32.h"

__declspec(dllimport) int __cdecl _vsnprintf(char *, size_t, const char *, va_list);
__declspec(dllimport) FILE *__cdecl __iob_func(void);
__declspec(dllimport) void __stdcall ExitProcess(unsigned);

int __cdecl gp3_vsnprintf(char *b, size_t n, const char *f, va_list ap) {
    if (!n) return 0;
    int r = _vsnprintf(b, n, f, ap);
    if (r < 0 || (size_t)r >= n) { b[n - 1] = 0; r = (int)n - 1; }
    return r;
}
int __cdecl gp3_snprintf(char *b, size_t n, const char *f, ...) {
    va_list ap; va_start(ap, f); int r = gp3_vsnprintf(b, n, f, ap); va_end(ap); return r;
}
FILE *gp3_stderr(void) { return (FILE *)((char *)__iob_func() + 2 * 32); }   /* &_iob[2], sizeof(FILE) == 32 */
void ExitProcess_(unsigned code) { ExitProcess(code); }

/* the compiler may emit calls to these for struct copies / large locals */
int _fltused = 0x9875;

/* _chkstk: called by compiler for frames > 4 KB. EAX = bytes to allocate.
 * Touches each 4 KB page in order (Windows grows the stack through its guard page),
 * then moves ESP down and returns. Not exported by msvcrt.dll, so it lives here. */
__attribute__((naked)) void _chkstk(void) {
    __asm__ volatile(
        "push %ecx\n"
        "lea 8(%esp), %ecx\n"          /* ecx = caller's esp (before the call) */
        "cmp $0x1000, %eax\n"
        "jb 2f\n"
        "1:\n"
        "sub $0x1000, %ecx\n"
        "test %eax, (%ecx)\n"          /* probe */
        "sub $0x1000, %eax\n"
        "cmp $0x1000, %eax\n"
        "jae 1b\n"
        "2:\n"
        "sub %eax, %ecx\n"
        "test %eax, (%ecx)\n"
        "mov %esp, %eax\n"             /* eax -> [saved ecx][return address] */
        "mov %ecx, %esp\n"
        "mov (%eax), %ecx\n"
        "push 4(%eax)\n"
        "ret\n");
}
