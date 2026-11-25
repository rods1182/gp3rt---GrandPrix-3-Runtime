/* ___chkstk_ms (libgcc) for the gcc-built SDL2 static library: touch each stack page
   below %esp down to %esp - %eax, keeping every register */
    .text
    .globl ___chkstk_ms
___chkstk_ms:
    pushl %ecx
    pushl %eax
    cmpl $0x1000, %eax
    leal 12(%esp), %ecx
    jb 2f
1:  subl $0x1000, %ecx
    orl $0, (%ecx)
    subl $0x1000, %eax
    cmpl $0x1000, %eax
    ja 1b
2:  subl %eax, %ecx
    orl $0, (%ecx)
    popl %eax
    popl %ecx
    ret
