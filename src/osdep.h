/* Small portability layer for the code shared by the Linux runner and the
 * Windows ddraw.dll (ddraw.c, d3d.c, rdr_*.c). */
#ifndef OSDEP_H
#define OSDEP_H
#include <stdint.h>
#ifdef _WIN32
typedef struct { void *debug; long lock_count, recursion; void *owner, *sem; unsigned long spin; } os_mutex_t;
__declspec(dllimport) void __stdcall InitializeCriticalSection(os_mutex_t *);
__declspec(dllimport) void __stdcall EnterCriticalSection(os_mutex_t *);
__declspec(dllimport) void __stdcall LeaveCriticalSection(os_mutex_t *);
static inline void os_mutex_init(os_mutex_t *m) { InitializeCriticalSection(m); }   /* recursive */
static inline void os_mutex_lock(os_mutex_t *m) { EnterCriticalSection(m); }
static inline void os_mutex_unlock(os_mutex_t *m) { LeaveCriticalSection(m); }
__declspec(dllimport) int __stdcall TryEnterCriticalSection(os_mutex_t *);
static inline int os_mutex_trylock(os_mutex_t *m) { return TryEnterCriticalSection(m) != 0; }
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
static inline unsigned os_thread_id(void) { return (unsigned)GetCurrentThreadId(); }
#else
#include <pthread.h>
typedef pthread_mutex_t os_mutex_t;
static inline void os_mutex_init(os_mutex_t *m) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
}
static inline void os_mutex_lock(os_mutex_t *m) { pthread_mutex_lock(m); }
static inline void os_mutex_unlock(os_mutex_t *m) { pthread_mutex_unlock(m); }
static inline int os_mutex_trylock(os_mutex_t *m) { return pthread_mutex_trylock(m) == 0; }
#include <unistd.h>
#include <sys/syscall.h>
static inline unsigned os_thread_id(void) { return (unsigned)syscall(SYS_gettid); }
#endif
void os_sleep_us(uint64_t us);
#endif
