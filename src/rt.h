/* gp3rt - Grand Prix 3 native runtime
 * Common definitions shared by the loader, the Win32 layer and the DirectX layer.
 */
#ifndef RT_H
#define RT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define WINAPI __attribute__((stdcall))
#define CDECL  __attribute__((cdecl))

typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t  LONG;
typedef uint32_t UINT;
typedef int32_t  BOOL;
typedef int32_t  HRESULT;
typedef uint32_t HANDLE;
typedef uint32_t HWND;
typedef uint32_t HMODULE;
typedef uint32_t WPARAM;
typedef int32_t  LPARAM;
typedef int32_t  LRESULT;
typedef uint16_t WCHAR;

#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE 0xffffffffu

/* ---- logging ---- */
extern int g_log_level;
extern FILE *g_log;
void rt_log(int lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOG(...)   rt_log(1, __VA_ARGS__)
#define TRACE(...) do { if (g_log_level >= 2) rt_log(2, __VA_ARGS__); } while (0)
#define WARN(...)  rt_log(0, __VA_ARGS__)
extern int g_trace_d3d;
#define DTRACE(...) do { if (g_trace_d3d) rt_log(0, __VA_ARGS__); } while (0)
void rt_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* ---- export registry ---- */
typedef struct {
    const char *dll;   /* lower-case without .dll */
    const char *name;  /* or NULL for ordinal-only */
    int ordinal;
    void *fn;
} export_t;
void rt_register_exports(const export_t *tab);
void *rt_find_export(const char *dll, const char *name, int ordinal);

/* ---- PE loader ---- */
typedef struct module {
    char name[64];       /* lower-case file name */
    uint32_t base, size, entry;
    int is_exe;
    struct module *next;
} module_t;
module_t *pe_load(const char *host_path, const char *name, int is_exe);
module_t *pe_find_module(const char *name);
module_t *pe_module_by_base(uint32_t base);
uint32_t pe_get_proc(module_t *m, const char *name, int ordinal);
extern module_t *g_exe;

/* ---- threads / TEB ---- */
typedef struct teb {
    uint32_t except_list;     /* 0x00 */
    uint32_t stack_base;      /* 0x04 */
    uint32_t stack_limit;     /* 0x08 */
    uint32_t sub_system_tib;  /* 0x0c */
    uint32_t fiber_data;      /* 0x10 */
    uint32_t arbitrary;       /* 0x14 */
    uint32_t self;            /* 0x18 */
    uint32_t env_ptr;         /* 0x1c */
    uint32_t pid;             /* 0x20 */
    uint32_t tid;             /* 0x24 */
    uint32_t rpc_handle;      /* 0x28 */
    uint32_t tls_ptr;         /* 0x2c */
    uint32_t peb;             /* 0x30 */
    uint32_t last_error;      /* 0x34 */
    uint8_t  pad[0xe10 - 0x38];
    uint32_t tls_slots[64];   /* 0xe10 */
    uint8_t  pad2[0x1000 - 0xf10];
} teb_t;
teb_t *teb_create(uint32_t stack_base, uint32_t stack_limit);
void teb_activate(teb_t *t);   /* load FS for the calling thread */
teb_t *teb_current(void);
void rt_install_signal_handlers(void);

/* ---- handles ---- */
enum { H_FILE = 1, H_EVENT, H_SEMAPHORE, H_MUTEX, H_THREAD, H_MAPPING, H_FIND, H_PROCESS, H_HEAP };
typedef struct hobj { int type; int refs; void *p; void (*close)(struct hobj *); } hobj_t;
HANDLE handle_new(int type, void *p, void (*close)(hobj_t *));
hobj_t *handle_get(HANDLE h, int type);
BOOL handle_close(HANDLE h);

/* ---- file system ---- */
#define VIRTUAL_CD_DRIVE 'd'   /* the game looks for its CD; we provide one */
extern char g_game_dir[1024];
int path_to_host(const char *win, char *out, size_t outsz);  /* returns 1 if file exists */
void cp1252_to_utf8(const char *in, char *out, size_t outsz);

/* ---- memory ---- */
void *rt_valloc(uint32_t addr, uint32_t size, int commit);
void rt_vfree(void *p, uint32_t size);
void mem_reserve_high(void);

/* ---- time ---- */
uint64_t rt_now_us(void);
uint32_t rt_ticks_ms(void);

/* ---- set last error ---- */
static inline void set_last_error(DWORD e) { teb_t *t = teb_current(); if (t) t->last_error = e; }

/* ---- string helpers ---- */
static inline int str_ieq(const char *a, const char *b) {
    while (*a && *b) { int x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; a++; b++; }
    return *a == *b;
}

/* ---- config ---- */
typedef struct {
    int headless;        /* no window: dump frames */
    int dump_every;      /* dump every Nth present (headless) */
    char dump_dir[512];
    int max_seconds;     /* exit after N seconds (testing) */
    char script[512];    /* input script */
    int scale;           /* window scale */
    int windowed;
} config_t;
extern config_t g_cfg;

/* ---- messages / window (implemented in user32.c) ---- */
void win_post_message(HWND h, UINT msg, WPARAM w, LPARAM l);
void win_input_key(int vk, int scancode, int down);
void win_input_char(int ch);
void win_input_mouse(int x, int y, int buttons_changed, int down);

#endif
