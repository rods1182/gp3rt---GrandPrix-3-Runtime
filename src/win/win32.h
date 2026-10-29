/* Minimal Win32 declarations used by the ddraw.dll platform layer
 * (keeps the build independent of the Windows SDK). */
#ifndef GP3_WIN32_H
#define GP3_WIN32_H
#include <stdint.h>
#include <stddef.h>
#include "../rt.h"          /* BYTE/WORD/DWORD/LONG/UINT/BOOL/HANDLE/HWND/HMODULE/WPARAM/LPARAM/LRESULT, WINAPI */
#define DLLIMP __declspec(dllimport)
typedef uint32_t HDC, HGLRC, HMONITOR, HBITMAP, HGDIOBJ, HINSTANCE, HCURSOR;
typedef LRESULT (WINAPI *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct { LONG left, top, right, bottom; } WRECT;
typedef struct { LONG x, y; } WPOINT;
typedef struct { DWORD cbSize; WRECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;
typedef struct { HWND hwnd, after; int x, y, cx, cy; UINT flags; } WINDOWPOS;
typedef struct {
    WORD nSize, nVersion; DWORD dwFlags; BYTE iPixelType, cColorBits, cRedBits, cRedShift, cGreenBits, cGreenShift,
    cBlueBits, cBlueShift, cAlphaBits, cAlphaShift, cAccumBits, cAccumRedBits, cAccumGreenBits, cAccumBlueBits,
    cAccumAlphaBits, cDepthBits, cStencilBits, cAuxBuffers, iLayerType, bReserved; DWORD dwLayerMask, dwVisibleMask, dwDamageMask;
} PIXELFORMATDESCRIPTOR;
typedef struct { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant; } BITMAPINFOHEADER;
typedef struct { BITMAPINFOHEADER h; DWORD colors[256]; } BITMAPINFO256;
typedef struct { DWORD code, flags; void *rec; void *address; DWORD nparams; uintptr_t info[15]; } EXCEPTION_RECORD_W;
typedef struct { EXCEPTION_RECORD_W *rec; void *ctx; } EXCEPTION_POINTERS_W;
typedef LONG (WINAPI *VEH_HANDLER)(EXCEPTION_POINTERS_W *);
typedef struct { DWORD orig_thunk, timestamp, fwd, name, first_thunk; } IMPORT_DESC;

/* kernel32 */
DLLIMP HMODULE WINAPI GetModuleHandleA(const char *);
DLLIMP DWORD WINAPI GetModuleFileNameA(HMODULE, char *, DWORD);
DLLIMP void *WINAPI GetProcAddress(HMODULE, const char *);
DLLIMP HMODULE WINAPI LoadLibraryA(const char *);
DLLIMP void *WINAPI VirtualAlloc(void *, size_t, DWORD, DWORD);
DLLIMP UINT WINAPI GetWriteWatch(DWORD, void *, size_t, void **, uintptr_t *, DWORD *);
DLLIMP BOOL WINAPI VirtualFree(void *, size_t, DWORD);
DLLIMP BOOL WINAPI VirtualProtect(void *, size_t, DWORD, DWORD *);
DLLIMP void *WINAPI AddVectoredExceptionHandler(DWORD, VEH_HANDLER);
DLLIMP BOOL WINAPI QueryPerformanceCounter(int64_t *);
DLLIMP BOOL WINAPI QueryPerformanceFrequency(int64_t *);
DLLIMP void WINAPI Sleep(DWORD);
DLLIMP void WINAPI OutputDebugStringA(const char *);
DLLIMP UINT WINAPI GetPrivateProfileIntA(const char *, const char *, int, const char *);
DLLIMP DWORD WINAPI GetPrivateProfileStringA(const char *, const char *, const char *, char *, DWORD, const char *);
DLLIMP BOOL WINAPI DisableThreadLibraryCalls(HMODULE);
DLLIMP DWORD WINAPI GetLogicalDrives(void);
DLLIMP UINT WINAPI GetDriveTypeA(const char *);
DLLIMP DWORD WINAPI GetLastError(void);
DLLIMP void WINAPI SetLastError(DWORD);
DLLIMP HANDLE WINAPI CreateWaitableTimerA(void *, BOOL, const char *);
DLLIMP BOOL WINAPI SetWaitableTimer(HANDLE, const int64_t *, LONG, void *, void *, BOOL);
DLLIMP DWORD WINAPI WaitForSingleObject(HANDLE, DWORD);
DLLIMP DWORD WINAPI GetFileAttributesA(const char *);
/* user32 */
DLLIMP LONG WINAPI SetWindowLongA(HWND, int, LONG);
DLLIMP LONG WINAPI GetWindowLongA(HWND, int);
DLLIMP LRESULT WINAPI CallWindowProcA(WNDPROC, HWND, UINT, WPARAM, LPARAM);
DLLIMP LRESULT WINAPI DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
DLLIMP BOOL WINAPI SetWindowPos(HWND, HWND, int, int, int, int, UINT);
DLLIMP BOOL WINAPI GetClientRect(HWND, WRECT *);
DLLIMP BOOL WINAPI GetWindowRect(HWND, WRECT *);
DLLIMP HMONITOR WINAPI MonitorFromWindow(HWND, DWORD);
DLLIMP BOOL WINAPI GetMonitorInfoA(HMONITOR, MONITORINFO *);
DLLIMP HDC WINAPI GetDC(HWND);
DLLIMP int WINAPI ReleaseDC(HWND, HDC);
DLLIMP BOOL WINAPI AdjustWindowRectEx(WRECT *, DWORD, BOOL, DWORD);
DLLIMP int WINAPI MessageBoxA(HWND, const char *, const char *, UINT);
DLLIMP BOOL WINAPI SetProcessDPIAware(void);
DLLIMP BOOL WINAPI IsWindow(HWND);
DLLIMP BOOL WINAPI InvalidateRect(HWND, const WRECT *, BOOL);
DLLIMP short WINAPI GetAsyncKeyState(int);
DLLIMP BOOL WINAPI ClipCursor(const WRECT *);
DLLIMP BOOL WINAPI ClientToScreen(HWND, WPOINT *);
DLLIMP HWND WINAPI GetForegroundWindow(void);
DLLIMP BOOL WINAPI SetForegroundWindow(HWND);
DLLIMP BOOL WINAPI BringWindowToTop(HWND);
DLLIMP HWND WINAPI SetFocus(HWND);
DLLIMP int WINAPI GetSystemMetrics(int);
DLLIMP DWORD WINAPI GetWindowThreadProcessId(HWND, DWORD *);
DLLIMP BOOL WINAPI PostMessageA(HWND, UINT, WPARAM, LPARAM);
typedef struct { UINT style; WNDPROC proc; int cls_extra, wnd_extra; HINSTANCE inst; uint32_t icon; HCURSOR cursor; uint32_t brush; const char *menu, *name; } WNDCLASSA_;
DLLIMP uint16_t WINAPI RegisterClassA(const WNDCLASSA_ *);
DLLIMP HWND WINAPI CreateWindowExA(DWORD, const char *, const char *, DWORD, int, int, int, int, HWND, uint32_t, HINSTANCE, void *);
DLLIMP BOOL WINAPI MoveWindow(HWND, int, int, int, int, BOOL);
DLLIMP BOOL WINAPI IsIconic(HWND);
DLLIMP HCURSOR WINAPI SetCursor(HCURSOR);
DLLIMP HCURSOR WINAPI LoadCursorA(HINSTANCE, const char *);
DLLIMP int WINAPI GetClassNameA(HWND, char *, int);
DLLIMP BOOL WINAPI AttachThreadInput(DWORD, DWORD, BOOL);
DLLIMP BOOL WINAPI ShowWindow(HWND, int);
DLLIMP DWORD WINAPI GetCurrentProcessId(void);
DLLIMP DWORD WINAPI GetTickCount(void);
/* gdi32 */
DLLIMP int WINAPI ChoosePixelFormat(HDC, const PIXELFORMATDESCRIPTOR *);
DLLIMP BOOL WINAPI SetPixelFormat(HDC, int, const PIXELFORMATDESCRIPTOR *);
DLLIMP BOOL WINAPI SwapBuffers(HDC);
DLLIMP HDC WINAPI CreateCompatibleDC(HDC);
DLLIMP HBITMAP WINAPI CreateDIBSection(HDC, const void *, UINT, void **, HANDLE, DWORD);
DLLIMP HGDIOBJ WINAPI SelectObject(HDC, HGDIOBJ);
DLLIMP BOOL WINAPI DeleteObject(HGDIOBJ);
DLLIMP BOOL WINAPI DeleteDC(HDC);
DLLIMP int WINAPI StretchDIBits(HDC, int, int, int, int, int, int, int, int, const void *, const void *, UINT, DWORD);
DLLIMP BOOL WINAPI GdiFlush(void);
DLLIMP BOOL WINAPI PatBlt(HDC, int, int, int, int, DWORD);
/* opengl32 */
DLLIMP HGLRC WINAPI wglCreateContext(HDC);
DLLIMP BOOL WINAPI wglMakeCurrent(HDC, HGLRC);
DLLIMP BOOL WINAPI wglDeleteContext(HGLRC);
DLLIMP void *WINAPI wglGetProcAddress(const char *);
DLLIMP HGLRC WINAPI wglGetCurrentContext(void);
/* winmm */
DLLIMP UINT WINAPI timeBeginPeriod(UINT);

#define GWL_WNDPROC (-4)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define WS_POPUP 0x80000000u
#define WS_VISIBLE 0x10000000u
#define WS_OVERLAPPEDWINDOW 0x00CF0000u
#define WS_CAPTION 0x00C00000u
#define WS_SYSMENU 0x00080000u
#define WS_MINIMIZEBOX 0x00020000u
#define SWP_NOZORDER 0x0004
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW 0x0040
#define HWND_TOP 0
#define MONITOR_DEFAULTTONEAREST 2
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#endif
