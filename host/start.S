/* Program entry that binds __libc_start_main to its oldest x86-64 version
   (GLIBC_2.2.5) instead of GLIBC_2.34, so the launcher also runs on older
   distributions (Mint 20 / Ubuntu 20.04 / Debian 11 ...). */
        .symver __libc_start_main_compat, __libc_start_main@GLIBC_2.2.5
        .text
        .globl _start
        .type _start, @function
_start:
        xor %ebp, %ebp
        mov %rdx, %r9            /* rtld_fini */
        pop %rsi                 /* argc */
        mov %rsp, %rdx           /* argv */
        and $-16, %rsp
        push %rax
        push %rsp
        xor %r8d, %r8d           /* fini */
        xor %ecx, %ecx           /* init */
        lea main(%rip), %rdi
        call *__libc_start_main_compat@GOTPCREL(%rip)
        hlt
        .section .note.GNU-stack,"",@progbits
