/* Thread environment: per-thread Windows TEB reachable through the FS segment
 * (via an LDT entry), and translation of Linux signals into Win32 SEH
 * exceptions dispatched along the fs:[0] handler chain. */
#include "rt.h"
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdatomic.h>

struct user_desc {
    unsigned int entry_number;
    unsigned int base_addr;
    unsigned int limit;
    unsigned int seg_32bit:1, contents:2, read_exec_only:1, limit_in_pages:1, seg_not_present:1, useable:1;
};

static __thread teb_t *t_teb;
static atomic_int g_next_ldt = 1;
static uint32_t g_peb_addr;
static atomic_uint g_next_tid = 0x100;

teb_t *teb_current(void) { return t_teb; }

teb_t *teb_create(uint32_t stack_base, uint32_t stack_limit) {
    if (!g_peb_addr) {
        uint8_t *peb = rt_valloc(0, 0x1000, 1);
        g_peb_addr = (uint32_t)(uintptr_t)peb;
        *(uint32_t *)(peb + 8) = g_exe ? g_exe->base : 0x400000;   /* ImageBaseAddress */
        peb[2] = 0;                                                  /* BeingDebugged */
    }
    teb_t *t = rt_valloc(0, sizeof(teb_t), 1);
    memset(t, 0, sizeof *t);
    t->except_list = 0xffffffffu;
    t->stack_base = stack_base;
    t->stack_limit = stack_limit;
    t->self = (uint32_t)(uintptr_t)t;
    t->pid = 0x1000 + (uint32_t)getpid() % 0x1000;
    t->tid = atomic_fetch_add(&g_next_tid, 4);
    t->peb = g_peb_addr;
    return t;
}

void teb_activate(teb_t *t) {
    struct user_desc d = {0};
    d.entry_number = atomic_fetch_add(&g_next_ldt, 1);
    d.base_addr = (uint32_t)(uintptr_t)t;
    d.limit = 0xfff;
    d.seg_32bit = 1;
    d.useable = 1;
    if (syscall(SYS_modify_ldt, 1, &d, sizeof d) != 0) rt_fatal("modify_ldt failed");
    unsigned short sel = (unsigned short)((d.entry_number << 3) | 7);
    __asm__ volatile("movw %0, %%fs" ::"r"(sel));
    t_teb = t;
}

/* ------------------------------------------------------------------ */
/* threads                                                              */
typedef DWORD (WINAPI *thread_fn)(void *);
struct tstart { thread_fn fn; void *arg; teb_t *teb; };

static void *thread_tramp(void *p) {
    struct tstart s = *(struct tstart *)p; free(p);
    char probe;
    uint32_t top = ((uint32_t)(uintptr_t)&probe + 0xfff) & ~0xfffu;
    s.teb->stack_base = top; s.teb->stack_limit = top - 0x200000;
    teb_activate(s.teb);
    { extern void rt_alt_stack(void); rt_alt_stack(); }
    DWORD r = s.fn(s.arg);
    return (void *)(uintptr_t)r;
}

int rt_thread_create(thread_fn fn, void *arg, DWORD *tid, pthread_t *out) {
    struct tstart *s = malloc(sizeof *s);
    s->fn = fn; s->arg = arg; s->teb = teb_create(0, 0);
    if (tid) *tid = s->teb->tid;
    pthread_attr_t a; pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 4 << 20);
    int r = pthread_create(out, &a, thread_tramp, s);
    pthread_attr_destroy(&a);
    return r;
}

/* ------------------------------------------------------------------ */
/* SEH                                                                  */
#pragma pack(push, 1)
typedef struct {
    DWORD code, flags; uint32_t record; uint32_t address; DWORD nparams; uint32_t info[15];
} EXCEPTION_RECORD;
typedef struct {
    DWORD flags;
    DWORD dr[6];
    BYTE  fsave[112];
    DWORD gs, fs, es, ds, edi, esi, ebx, edx, ecx, eax, ebp, eip, cs, eflags, esp, ss;
    BYTE  ext[512];
} CONTEXT;
typedef struct seh_frame { uint32_t next; uint32_t handler; } seh_frame;
#pragma pack(pop)

typedef int (CDECL *seh_handler_t)(EXCEPTION_RECORD *, void *frame, CONTEXT *, void *disp);

/* resume execution with the given context */
__attribute__((naked, noreturn)) static void restore_context(CONTEXT *c) {
    __asm__ volatile(
        "mov 4(%esp), %ecx\n"
        "frstor 0x1c(%ecx)\n"
        "mov 0xc4(%ecx), %eax\n"     /* target esp */
        "sub $16, %eax\n"
        "mov 0xb8(%ecx), %edx\n mov %edx, 12(%eax)\n"   /* eip */
        "mov 0xc0(%ecx), %edx\n mov %edx, 8(%eax)\n"    /* eflags */
        "mov 0xac(%ecx), %edx\n mov %edx, 4(%eax)\n"    /* ecx */
        "mov 0xb0(%ecx), %edx\n mov %edx, 0(%eax)\n"    /* eax */
        "mov 0xa4(%ecx), %ebx\n"
        "mov 0xa8(%ecx), %edx\n"
        "mov 0xa0(%ecx), %esi\n"
        "mov 0x9c(%ecx), %edi\n"
        "mov 0xb4(%ecx), %ebp\n"
        "mov %eax, %esp\n"
        "pop %eax\n pop %ecx\n popfl\n ret\n");
}

static void dump_and_die(EXCEPTION_RECORD *er, CONTEXT *c) {
    WARN("*** unhandled exception %08x at eip=%08x (addr param %08x)", er->code, c->eip, er->info[1]);
    WARN("    eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
         c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->ebp, c->esp);
    uint32_t *sp = (uint32_t *)(uintptr_t)c->esp;
    WARN("    stack: %08x %08x %08x %08x %08x %08x %08x %08x", sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7]);
    /* walk ebp chain */
    uint32_t bp = c->ebp;
    for (int i = 0; i < 12 && bp > 0x10000 && bp < 0xfffff000u; i++) {
        uint32_t *f = (uint32_t *)(uintptr_t)bp;
        WARN("    frame %d: ret=%08x", i, f[1]);
        if (f[0] <= bp) break;
        bp = f[0];
    }
    if (g_log) fflush(g_log);
    _exit(3);
}

int g_seh_trace;

/* runs on the faulting thread's stack after the signal handler returned */
static void CDECL seh_dispatch(EXCEPTION_RECORD *er, CONTEXT *c) {
    teb_t *t = t_teb;
    uint32_t fr = t ? t->except_list : 0xffffffffu;
    if (g_seh_trace || getenv("GP3_SEH_TRACE")) {
        LOG("SEH: exception %08x at %08x (read/write %08x) esp %08x eax %08x ecx %08x edx %08x esi %08x", er->code, er->address, er->info[1], c->esp, c->eax, c->ecx, c->edx, c->esi);
        { extern void gpx_dump_modules(void); static int dumped; if (!dumped++ && getenv("GP3_GPX_DUMP")) gpx_dump_modules(); }
        { const uint32_t *sp = (const uint32_t *)(uintptr_t)c->esp; LOG("SEH:   stack %08x %08x %08x %08x %08x %08x %08x %08x", sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7]); }
        for (uint32_t q = fr; q && q != 0xffffffffu; q = ((seh_frame *)(uintptr_t)q)->next)
            LOG("SEH:   frame %08x handler %08x", q, ((seh_frame *)(uintptr_t)q)->handler);
    }
    while (fr && fr != 0xffffffffu) {
        seh_frame *f = (seh_frame *)(uintptr_t)fr;
        uint32_t disp = 0;
        int r = ((seh_handler_t)(uintptr_t)f->handler)(er, f, c, &disp);
        if (r == 0) restore_context(c);          /* ExceptionContinueExecution */
        if (r != 1) break;                       /* nested / collided: give up */
        fr = f->next;
    }
    dump_and_die(er, c);
}

/* a software exception (C++ throw from a GPx module): offered to the SEH frames of this
   thread like a hardware one; returns only when no frame took it */
void seh_raise_sw(uint32_t code, uint32_t nparams, const uint32_t *params, uint32_t addr) {
    static __thread EXCEPTION_RECORD er; static __thread CONTEXT c;
    memset(&er, 0, sizeof er); memset(&c, 0, sizeof c);
    er.code = code; er.flags = 1; er.address = addr; er.nparams = nparams;
    for (uint32_t i = 0; i < nparams && i < 15; i++) er.info[i] = params[i];
    teb_t *t = t_teb;
    uint32_t fr = t ? t->except_list : 0xffffffffu;
    while (fr && fr != 0xffffffffu) {
        seh_frame *f = (seh_frame *)(uintptr_t)fr;
        uint32_t disp = 0;
        int r = ((seh_handler_t)(uintptr_t)f->handler)(&er, f, &c, &disp);
        if (r != 1) break;
        fr = f->next;
    }
}

/* diagnostics: trace every access to a surface (page fault + single step) */
uint8_t *g_acc_base; size_t g_acc_len; FILE *g_acc_file; long g_acc_count;
static uint8_t *g_acc_page; static uint32_t g_acc_addr; static int g_acc_w;
static void sig_handler_body(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    greg_t *g = uc->uc_mcontext.gregs;
    uint32_t esp = (uint32_t)g[REG_ESP];
    if (sig == SIGSEGV && g_acc_base && (uint8_t *)si->si_addr >= g_acc_base && (uint8_t *)si->si_addr < g_acc_base + g_acc_len) {
        uint8_t *a = si->si_addr;
        g_acc_page = (uint8_t *)((uintptr_t)a & ~(uintptr_t)4095);
        mprotect(g_acc_page, 4096, PROT_READ | PROT_WRITE);
        g_acc_addr = (uint32_t)(uintptr_t)a & ~1u; g_acc_w = (g[REG_ERR] & 2) != 0;
        if (g_acc_file && g_acc_count < 3000000) {
            uint32_t rec[4] = { (uint32_t)g[REG_EIP], g_acc_addr, (uint32_t)g_acc_w, *(uint16_t *)(uintptr_t)g_acc_addr };
            fwrite(rec, 4, 4, g_acc_file); g_acc_count++;
        }
        g[REG_EFL] |= 0x100;   /* trap after this instruction */
        return;
    }
    {   /* analysis: GP3_BP breakpoints (any instruction): log registers + stack, step over, re-arm */
        extern uint32_t g_bp[16]; extern uint8_t g_bp_orig[16]; extern int g_nbp; static __thread int rearm = -1;
        if (sig == SIGTRAP && rearm >= 0) {
            uint8_t cc = 0xcc; extern int plat_patch_code(void *, const void *, int);
            plat_patch_code((void *)(uintptr_t)g_bp[rearm], &cc, 1); rearm = -1; g[REG_EFL] &= ~0x100; return;
        }
        if (sig == SIGTRAP && g_nbp) {
            uint32_t site = (uint32_t)g[REG_EIP] - 1;
            for (int i = 0; i < g_nbp; i++) if (g_bp[i] == site) {
                static int n; uint32_t *sp = (uint32_t *)(uintptr_t)esp;
                if (n++ < 400) {
                    LOG("bp %08x: eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x", site, (uint32_t)g[REG_EAX], (uint32_t)g[REG_EBX], (uint32_t)g[REG_ECX], (uint32_t)g[REG_EDX], (uint32_t)g[REG_ESI], (uint32_t)g[REG_EDI], (uint32_t)g[REG_EBP], esp);
                    LOG("bp %08x: stack %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", site, sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7], sp[8], sp[9]);
                }
                extern int plat_patch_code(void *, const void *, int);
                plat_patch_code((void *)(uintptr_t)site, &g_bp_orig[i], 1);
                g[REG_EIP] = site; g[REG_EFL] |= 0x100; rearm = i; return;
            }
        }
    }
    if (sig == SIGTRAP && !g_acc_page) {
        uint32_t site = (uint32_t)g[REG_EIP] - 1;     /* int3 already executed */
        extern int g_native_ddraw;
        { extern uint32_t g_traps[16]; extern int g_ntraps;   /* analysis: GP3_TRAP (int3 placed on a 1-byte nop) */
          for (int i = 0; i < g_ntraps; i++) if (g_traps[i] == site) { static int n; if (n++ < 200) LOG("trap %08x hit (eax=%08x ecx=%08x esp=%08x)", site, (uint32_t)g[REG_EAX], (uint32_t)g[REG_ECX], esp); return; } }
        /* GP3's own assertions are int3 instructions followed by the normal code path. Under
           GPxPatch (a debugger) they are reported and the game simply continues; many community
           tracks exceed the original limits and trip them ("needs GPxPatch debug mode"). Same here,
           unless GP3_STRICT_ASSERTS is set ([game] skip_asserts=0). */
        { static int ign = -1; if (ign < 0) ign = getenv("GP3_STRICT_ASSERTS") == NULL;
          if (ign && !g_native_ddraw && site >= 0x401000 && site < 0x900000 && *(uint8_t *)(uintptr_t)site == 0xcc && !({ extern int d3d_is_shadow_site(uint32_t a); d3d_is_shadow_site(site); })) {
              static int n; if (n++ < 20) LOG("GP3 assertion at %08x ignored (as GPxPatch does)", site); return; } }
        if (!g_native_ddraw) {
            extern int d3d_is_shadow_site(uint32_t a); extern int d3d_shadow_read(uint32_t addr, uint8_t *out);
            uint8_t v;
            if (d3d_is_shadow_site(site) && d3d_shadow_read((uint32_t)g[REG_EDI], &v)) {
                g[REG_ECX] = (g[REG_ECX] & ~0xff) | v; g[REG_EIP] = site + 2; return;
            }
        } else {
            extern uint32_t g_veh[8]; extern int g_nveh;
            EXCEPTION_RECORD er; memset(&er, 0, sizeof er);
            er.code = 0x80000003; er.address = site;
            CONTEXT ctx; memset(&ctx, 0, sizeof ctx);
            ctx.eip = site; ctx.esp = (uint32_t)g[REG_ESP]; ctx.edi = (uint32_t)g[REG_EDI]; ctx.esi = (uint32_t)g[REG_ESI];
            ctx.ecx = (uint32_t)g[REG_ECX]; ctx.eax = (uint32_t)g[REG_EAX]; ctx.ebx = (uint32_t)g[REG_EBX]; ctx.edx = (uint32_t)g[REG_EDX];
            ctx.ebp = (uint32_t)g[REG_EBP]; ctx.eflags = (uint32_t)g[REG_EFL];
            uint32_t ptrs[2] = { (uint32_t)(uintptr_t)&er, (uint32_t)(uintptr_t)&ctx };
            for (int i = 0; i < g_nveh; i++) {
                typedef LONG (WINAPI *veh_t)(void *);
                if (((veh_t)(uintptr_t)g_veh[i])(ptrs) == -1) { g[REG_ECX] = ctx.ecx; g[REG_EIP] = ctx.eip; return; }
            }
        }
    }
    if (sig == SIGTRAP && g_acc_page) {
        if (g_acc_w && g_acc_file && g_acc_count < 3000000) {   /* value after the write */
            uint32_t rec[4] = { 0xffffffffu, g_acc_addr, 2, *(uint16_t *)(uintptr_t)g_acc_addr };
            fwrite(rec, 4, 4, g_acc_file);
        }
        if (g_acc_base) mprotect(g_acc_page, 4096, PROT_NONE);
        g_acc_page = NULL;
        g[REG_EFL] &= ~0x100;
        return;
    }
    if (sig == SIGSEGV) {
        extern int d3d_guard_fault(void *addr, int is_write);
        { extern uint32_t g_fault_eip; g_fault_eip = (uint32_t)g[REG_EIP]; }
        if (d3d_guard_fault(si->si_addr, (g[REG_ERR] & 2) != 0)) return;   /* lazy readback / write tracking */
        /* vectored exception handlers registered by loaded DLLs (e.g. the Windows ddraw.dll) */
        extern uint32_t g_veh[8]; extern int g_nveh;
        if (g_nveh) {
            EXCEPTION_RECORD er; memset(&er, 0, sizeof er);
            er.code = 0xC0000005; er.address = (uint32_t)g[REG_EIP]; er.nparams = 2;
            er.info[0] = (g[REG_ERR] & 2) ? 1 : 0; er.info[1] = (uint32_t)(uintptr_t)si->si_addr;
            CONTEXT ctx; memset(&ctx, 0, sizeof ctx); ctx.eip = (uint32_t)g[REG_EIP]; ctx.esp = (uint32_t)g[REG_ESP];
            uint32_t ptrs[2] = { (uint32_t)(uintptr_t)&er, (uint32_t)(uintptr_t)&ctx };
            for (int i = 0; i < g_nveh; i++) {
                typedef LONG (WINAPI *veh_t)(void *);
                if (((veh_t)(uintptr_t)g_veh[i])(ptrs) == -1) return;   /* EXCEPTION_CONTINUE_EXECUTION */
            }
        }
    }
    if (!t_teb) {
        WARN("fatal signal %d at eip=%08x (no TEB)", sig, (uint32_t)g[REG_EIP]);
        _exit(4);
    }
    /* build frame below the faulting esp */
    uint32_t sp = (esp - 0x200 - sizeof(CONTEXT) - sizeof(EXCEPTION_RECORD)) & ~15u;
    CONTEXT *c = (CONTEXT *)(uintptr_t)sp;
    EXCEPTION_RECORD *er = (EXCEPTION_RECORD *)(uintptr_t)(sp + sizeof(CONTEXT));
    memset(c, 0, sizeof *c); memset(er, 0, sizeof *er);
    c->flags = 0x1001f;
    c->gs = g[REG_GS]; c->fs = g[REG_FS]; c->es = g[REG_ES]; c->ds = g[REG_DS];
    c->edi = g[REG_EDI]; c->esi = g[REG_ESI]; c->ebx = g[REG_EBX]; c->edx = g[REG_EDX];
    c->ecx = g[REG_ECX]; c->eax = g[REG_EAX]; c->ebp = g[REG_EBP]; c->eip = g[REG_EIP];
    c->cs = g[REG_CS]; c->eflags = g[REG_EFL]; c->esp = esp; c->ss = g[REG_SS];
    if (uc->uc_mcontext.fpregs) {
        /* fpregs is in fsave layout (struct _libc_fpstate starts with env + 8 regs of 10 bytes) */
        memcpy(c->fsave, uc->uc_mcontext.fpregs, 108);
    }
    er->address = c->eip;
    switch (sig) {
    case SIGSEGV: case SIGBUS:
        er->code = 0xC0000005; er->nparams = 2;
        er->info[0] = (g[REG_ERR] & 2) ? 1 : 0;
        er->info[1] = (uint32_t)(uintptr_t)si->si_addr;
        break;
    case SIGFPE:
        switch (si->si_code) {
        case FPE_INTDIV: er->code = 0xC0000094; break;
        case FPE_INTOVF: er->code = 0xC0000095; break;
        case FPE_FLTDIV: er->code = 0xC000008E; break;
        case FPE_FLTOVF: er->code = 0xC0000091; break;
        case FPE_FLTUND: er->code = 0xC0000093; break;
        case FPE_FLTRES: er->code = 0xC000008F; break;
        default:         er->code = 0xC0000090; break;
        }
        break;
    case SIGILL: er->code = 0xC000001D; break;
    case SIGTRAP: er->code = 0x80000003; break;
    default: er->code = 0xC0000005;
    }
    /* call seh_dispatch(er, c) with a fake return address */
    uint32_t *nsp = (uint32_t *)(uintptr_t)(sp - 16);
    nsp[0] = 0; nsp[1] = (uint32_t)(uintptr_t)er; nsp[2] = (uint32_t)(uintptr_t)c;
    g[REG_ESP] = (greg_t)(uintptr_t)nsp;
    g[REG_EIP] = (greg_t)(uintptr_t)seh_dispatch;
    /* make sure our C code runs with a sane FPU control word / no pending exceptions */
    if (uc->uc_mcontext.fpregs) {
        uc->uc_mcontext.fpregs->cw = 0x37f;
        uc->uc_mcontext.fpregs->sw = 0;
    }
}

/* every thread gets its own signal stack: a fault with a broken or overflowed stack still
   reaches the handler (and the log) instead of the kernel killing the process silently */
void rt_alt_stack(void) {
    stack_t ss = { .ss_sp = malloc(256 << 10), .ss_size = 256 << 10, .ss_flags = 0 };
    if (ss.ss_sp) sigaltstack(&ss, NULL);
}
static __thread int t_in_sig;
static void sig_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx; greg_t *g = uc->uc_mcontext.gregs;
    if (t_in_sig) {   /* faulted again while preparing the exception: the stack is unusable */
        WARN("*** fatal signal %d while handling a fault: eip=%08x esp=%08x addr=%08x (stack overflow or corrupted stack?)",
             sig, (uint32_t)g[REG_EIP], (uint32_t)g[REG_ESP], (uint32_t)(uintptr_t)si->si_addr);
        if (t_teb) WARN("    thread stack %08x-%08x", t_teb->stack_limit, t_teb->stack_base);
        if (g_log) fflush(g_log);
        _exit(5);
    }
    t_in_sig = 1;
    uint32_t esp = (uint32_t)g[REG_ESP];
    if (t_teb && t_teb->stack_base && (esp > t_teb->stack_base || esp < t_teb->stack_limit + 0x1000) && sig == SIGSEGV) {
        WARN("*** fault with the stack outside its limits: eip=%08x esp=%08x addr=%08x stack %08x-%08x (stack overflow?)",
             (uint32_t)g[REG_EIP], esp, (uint32_t)(uintptr_t)si->si_addr, t_teb->stack_limit, t_teb->stack_base);
        if (g_log) fflush(g_log);
    }
    sig_handler_body(sig, si, ctx);
    t_in_sig = 0;
}

void rt_install_signal_handlers(void) {
    rt_alt_stack();
    struct sigaction sa = {0};
    sa.sa_sigaction = sig_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
}

/* RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue) */
uint32_t WINAPI k32_RtlUnwind(void *target, void *ip, EXCEPTION_RECORD *er, uint32_t retval) {
    teb_t *t = t_teb;
    EXCEPTION_RECORD local = {0};
    if (!er) { er = &local; er->code = 0xC0000027; }
    er->flags |= 2; /* EXCEPTION_UNWINDING */
    if (!target) er->flags |= 4; /* EXIT_UNWIND */
    uint32_t fr = t->except_list;
    CONTEXT dummy; memset(&dummy, 0, sizeof dummy);
    while (fr != 0xffffffffu && fr != (uint32_t)(uintptr_t)target) {
        seh_frame *f = (seh_frame *)(uintptr_t)fr;
        uint32_t disp = 0;
        ((seh_handler_t)(uintptr_t)f->handler)(er, f, &dummy, &disp);
        fr = f->next;
        t->except_list = fr;
    }
    (void)ip;
    return retval;
}
