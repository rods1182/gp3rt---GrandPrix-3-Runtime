/* PE image loader: maps Gp3.exe (and real DLLs such as binkw32.dll) into the
 * 32-bit address space and resolves their imports against our own Win32/DirectX
 * implementation (export registry) or against other loaded PE modules. */
#include "rt.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

module_t *g_exe;
module_t *g_modules;

/* ------------------------------------------------------------------ */
/* export registry                                                      */
#define MAX_TABS 64
static const export_t *g_tabs[MAX_TABS];
static int g_ntabs;

void rt_register_exports(const export_t *tab) {
    if (g_ntabs < MAX_TABS) g_tabs[g_ntabs++] = tab;
}

static void norm_dll(const char *in, char *out, size_t n) {
    size_t i = 0;
    const char *s = strrchr(in, '\\'); if (s) in = s + 1;
    s = strrchr(in, '/'); if (s) in = s + 1;
    for (; *in && i + 1 < n; in++) out[i++] = (char)tolower((unsigned char)*in);
    out[i] = 0;
    char *dot = strrchr(out, '.');
    if (dot && (!strcmp(dot, ".dll") || !strcmp(dot, ".drv"))) *dot = 0;
}

void *rt_find_export(const char *dll, const char *name, int ordinal) {
    char d[64]; norm_dll(dll, d, sizeof d);
    for (int t = 0; t < g_ntabs; t++)
        for (const export_t *e = g_tabs[t]; e->dll; e++) {
            if (strcmp(e->dll, d)) continue;
            if (name && e->name && !strcmp(e->name, name)) return e->fn;
            if (!name && ordinal && e->ordinal == ordinal) return e->fn;
        }
    return NULL;
}

int dll_has_builtins(const char *dn) {
    for (int t = 0; t < g_ntabs; t++)
        for (const export_t *e = g_tabs[t]; e->dll; e++) if (!strcmp(e->dll, dn)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* thunks for unimplemented imports: push id ; jmp unimpl_trap          */
typedef struct { char dll[32]; char name[64]; } missing_t;
static missing_t g_missing[1024];
static int g_nmissing;
static uint8_t *g_thunk_mem;
static int g_thunk_used;

__attribute__((used)) void unimpl_report(uint32_t id, uint32_t retaddr) {
    missing_t *m = &g_missing[id];
    rt_fatal("Unimplemented API called: %s!%s (from 0x%08x)", m->dll, m->name, retaddr);
}
/* entry: [esp]=id, [esp+4]=return address into game */
__attribute__((naked)) static void unimpl_trap(void) {
    __asm__ volatile(
        "pop %eax\n"
        "mov (%esp), %ecx\n"
        "and $-16, %esp\n"
        "sub $8, %esp\n"
        "push %ecx\n"
        "push %eax\n"
        "call unimpl_report\n"
        "hlt\n");
}

void *make_missing_thunk(const char *dll, const char *name) {
    if (!g_thunk_mem) {
        g_thunk_mem = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    int id = g_nmissing++;
    snprintf(g_missing[id].dll, sizeof g_missing[id].dll, "%s", dll);
    snprintf(g_missing[id].name, sizeof g_missing[id].name, "%s", name);
    uint8_t *p = g_thunk_mem + g_thunk_used; g_thunk_used += 16;
    p[0] = 0x68; memcpy(p + 1, &id, 4);                    /* push id */
    int32_t rel = (int32_t)((uintptr_t)unimpl_trap - (uintptr_t)(p + 10));
    p[5] = 0xe9; memcpy(p + 6, &rel, 4);                   /* jmp unimpl_trap */
    return p;
}

/* ------------------------------------------------------------------ */
/* minimal PE structures                                                */
#pragma pack(push, 1)
typedef struct { WORD magic; BYTE pad[58]; LONG lfanew; } dos_hdr;
typedef struct { WORD machine, nsect; DWORD timestamp, symptr, nsym; WORD opt_size, chars; } file_hdr;
typedef struct { DWORD va, size; } data_dir;
typedef struct {
    WORD magic; BYTE lmaj, lmin; DWORD code_size, idata_size, udata_size, entry, code_base, data_base, image_base;
    DWORD sect_align, file_align; WORD os_maj, os_min, img_maj, img_min, ss_maj, ss_min; DWORD win32ver, image_size,
    hdr_size, checksum; WORD subsystem, dll_chars; DWORD stack_res, stack_com, heap_res, heap_com, loader_flags, ndirs;
    data_dir dirs[16];
} opt_hdr;
typedef struct { char name[8]; DWORD vsize, va, raw_size, raw_ptr, reloc_ptr, lines_ptr; WORD nrel, nlines; DWORD chars; } sect_hdr;
typedef struct { DWORD orig_thunk, timestamp, fwd, name, first_thunk; } import_desc;
typedef struct { DWORD chars, timestamp; WORD maj, min; DWORD name, base, nfuncs, nnames, funcs, names, ords; } export_dir;
#pragma pack(pop)

static void resolve_imports(module_t *m, uint32_t base, opt_hdr *oh) {
    data_dir *dd = &oh->dirs[1];
    if (!dd->va) return;
    for (import_desc *id = (import_desc *)(base + dd->va); id->name; id++) {
        const char *dll = (const char *)(base + id->name);
        char dn[64]; norm_dll(dll, dn, sizeof dn);
        module_t *pm = pe_find_module(dll);
        if (!pm && !dll_has_builtins(dn)) {
            /* a real PE dll shipped with the game (e.g. binkw32.dll) */
            char hp[1024];
            if (path_to_host(dll, hp, sizeof hp)) pm = pe_load(hp, dll, 0);
        }
        uint32_t *iat = (uint32_t *)(base + id->first_thunk);
        uint32_t *ilt = (uint32_t *)(base + (id->orig_thunk ? id->orig_thunk : id->first_thunk));
        for (int i = 0; ilt[i]; i++) {
            const char *name = NULL; int ord = 0;
            char nb[32];
            if (ilt[i] & 0x80000000u) { ord = ilt[i] & 0xffff; snprintf(nb, sizeof nb, "#%d", ord); }
            else name = (const char *)(base + ilt[i] + 2);
            void *fn = NULL;
            if (pm) fn = (void *)(uintptr_t)pe_get_proc(pm, name, ord);
            if (!fn) fn = rt_find_export(dll, name, ord);
            if (!fn) {
                fn = make_missing_thunk(dn, name ? name : nb);
                TRACE("  import %s!%s -> UNIMPLEMENTED", dn, name ? name : nb);
            }
            iat[i] = (uint32_t)(uintptr_t)fn;
        }
    }
}

static void apply_relocs(uint32_t base, opt_hdr *oh, int32_t delta) {
    data_dir *dd = &oh->dirs[5];
    if (!delta || !dd->va) return;
    uint8_t *p = (uint8_t *)(base + dd->va), *end = p + dd->size;
    while (p < end) {
        DWORD page = *(DWORD *)p, sz = *(DWORD *)(p + 4);
        if (!sz) break;
        WORD *e = (WORD *)(p + 8);
        for (DWORD i = 0; i < (sz - 8) / 2; i++) {
            int type = e[i] >> 12, off = e[i] & 0xfff;
            if (type == 3) *(uint32_t *)(base + page + off) += delta;
        }
        p += sz;
    }
}

module_t *pe_load(const char *host_path, const char *name, int is_exe) {
    int fd = open(host_path, O_RDONLY);
    if (fd < 0) { WARN("pe_load: cannot open %s", host_path); return NULL; }
    off_t fsz = lseek(fd, 0, SEEK_END);
    uint8_t *file = malloc(fsz);
    pread(fd, file, fsz, 0); close(fd);
    dos_hdr *dh = (dos_hdr *)file;
    if (dh->magic != 0x5a4d) { free(file); return NULL; }
    file_hdr *fh = (file_hdr *)(file + dh->lfanew + 4);
    opt_hdr *oh = (opt_hdr *)(fh + 1);
    sect_hdr *sh = (sect_hdr *)((uint8_t *)oh + fh->opt_size);
    uint32_t size = (oh->image_size + 0xfff) & ~0xfffu;
    void *want = (void *)(uintptr_t)oh->image_base;
    void *mem = mmap(want, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | (is_exe ? MAP_FIXED : MAP_FIXED_NOREPLACE), -1, 0);
    if (mem == MAP_FAILED && !is_exe)
        mem = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) rt_fatal("pe_load: cannot map %s at %p (%s)", name, want, strerror(errno));
    uint32_t base = (uint32_t)(uintptr_t)mem;
    memcpy(mem, file, oh->hdr_size);
    for (int i = 0; i < fh->nsect; i++) {
        uint32_t n = sh[i].raw_size < sh[i].vsize || !sh[i].vsize ? sh[i].raw_size : sh[i].vsize;
        if (sh[i].raw_ptr && n) memcpy((uint8_t *)mem + sh[i].va, file + sh[i].raw_ptr, n);
    }
    opt_hdr *moh = (opt_hdr *)((uint8_t *)mem + ((uint8_t *)oh - file));
    apply_relocs(base, moh, (int32_t)(base - oh->image_base));
    module_t *m = calloc(1, sizeof *m);
    char dn[64]; norm_dll(name, dn, sizeof dn);
    snprintf(m->name, sizeof m->name, "%s", dn);
    m->base = base; m->size = size; m->entry = oh->entry ? base + oh->entry : 0; m->is_exe = is_exe;
    m->next = g_modules; g_modules = m;
    if (is_exe) g_exe = m;
    LOG("loaded %s at 0x%08x size 0x%x entry 0x%08x", name, base, size, m->entry);
    resolve_imports(m, base, moh);
    free(file);
    if (!is_exe && m->entry) {
        typedef BOOL (WINAPI *dllmain_t)(uint32_t, DWORD, void *);
        LOG("calling DllMain of %s", m->name);
        if (!((dllmain_t)(uintptr_t)m->entry)(base, 1, NULL)) LOG("DllMain of %s returned FALSE", m->name);
        { extern void bp_install_range(uint32_t lo, uint32_t hi); bp_install_range(base, base + size); }
        const char *dd = getenv("GP3_DUMPMOD");   /* debugging: image after DllMain (unpacked modules) */
        if (dd) { char fn[512]; snprintf(fn, sizeof fn, "%s/%s.img", dd, m->name); FILE *df = fopen(fn, "wb");
            if (df) { fwrite((void *)(uintptr_t)base, 1, size, df); fclose(df); } }
    }
    return m;
}

module_t *pe_find_module(const char *name) {
    char dn[64]; norm_dll(name, dn, sizeof dn);
    for (module_t *m = g_modules; m; m = m->next) if (!strcmp(m->name, dn)) return m;
    return NULL;
}
module_t *pe_module_by_addr(uint32_t a) {
    for (module_t *m = g_modules; m; m = m->next) if (a >= m->base && a < m->base + m->size) return m;
    return NULL;
}
module_t *pe_module_by_base(uint32_t base) {
    for (module_t *m = g_modules; m; m = m->next) if (m->base == base) return m;
    return NULL;
}

uint32_t pe_get_proc(module_t *m, const char *name, int ordinal) {
    dos_hdr *dh = (dos_hdr *)(uintptr_t)m->base;
    opt_hdr *oh = (opt_hdr *)((uint8_t *)(uintptr_t)m->base + dh->lfanew + 4 + sizeof(file_hdr));
    data_dir *dd = &oh->dirs[0];
    if (!dd->va) return 0;
    export_dir *ed = (export_dir *)(uintptr_t)(m->base + dd->va);
    uint32_t *funcs = (uint32_t *)(uintptr_t)(m->base + ed->funcs);
    uint32_t *names = (uint32_t *)(uintptr_t)(m->base + ed->names);
    uint16_t *ords = (uint16_t *)(uintptr_t)(m->base + ed->ords);
    if (!name) {
        uint32_t idx = ordinal - ed->base;
        return idx < ed->nfuncs ? m->base + funcs[idx] : 0;
    }
    for (uint32_t i = 0; i < ed->nnames; i++)
        if (!strcmp((const char *)(uintptr_t)(m->base + names[i]), name)) return m->base + funcs[ords[i]];
    return 0;
}

/* module containing an address (diagnostics) */
const char *rt_module_name_of(uint32_t a, uint32_t *base) {
    for (module_t *m = g_modules; m; m = m->next)
        if (a >= m->base && a < m->base + m->size) { if (base) *base = m->base; return m->name; }
    if (base) *base = 0;
    return NULL;
}
