@ Memory: brk, mmap2 (anonymous, fixed), mprotect, munmap, mremap.
    .include "common.S"
    .text
    .global _start
_start:
    @ brk(0) -> base (page aligned?) ; brk(base + 0x3000) ; store/load ; shrink
    mov r0, #0
    SYS 45
    mov r4, r0
    ldr r1, =0xFFF
    and r0, r0, r1
    bl puthex               @ 0: the break is page aligned at start
    add r0, r4, #0x3000
    SYS 45
    sub r0, r0, r4
    bl puthex               @ 3000
    ldr r1, =0x12345678
    add r2, r4, #0x2F00
    str r1, [r2, #0xF0]
    ldr r0, [r2, #0xF0]
    bl puthex
    mov r0, r4
    SYS 45
    sub r0, r0, r4
    bl puthex               @ 0
    @ mmap2 anonymous 2 pages RW
    mov r0, #0
    mov r1, #0x2000
    mov r2, #3
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    SYS 192
    mov r6, r0
    ldr r1, =0xFFF
    and r0, r0, r1
    bl puthex               @ 0
    ldr r1, =0xCAFEBABE
    add r2, r6, #0x1F00
    str r1, [r2, #0xFC]
    ldr r0, [r2, #0xFC]
    bl puthex
    ldr r0, [r6]
    bl puthex               @ 0: zero filled
    @ mprotect the first page read-only
    mov r0, r6
    mov r1, #0x1000
    mov r2, #1
    SYS 125
    bl puthex               @ 0
    @ fixed mapping at 0x30000000
    ldr r0, =0x30000000
    mov r1, #0x1000
    mov r2, #3
    mov r3, #0x32
    mvn r4, #0
    mov r5, #0
    SYS 192
    bl puthex               @ 30000000
    @ mremap grow (may move): old 0x1000 at the fixed page -> 0x3000
    ldr r0, =0x30000000
    ldr r1, =0x11223344
    str r1, [r0]
    mov r1, #0x1000
    mov r2, #0x3000
    mov r3, #1
    SYS 163
    mov r8, r0
    ldr r0, [r8]
    bl puthex               @ 11223344 preserved
    ldr r1, =0x55667788
    add r2, r8, #0x2F00
    str r1, [r2, #0xF8]
    ldr r0, [r2, #0xF8]
    bl puthex
    mov r0, r8
    mov r1, #0x3000
    SYS 91
    bl puthex               @ 0
    @ munmap the anonymous pages, and again (0)
    mov r0, r6
    mov r1, #0x2000
    SYS 91
    bl puthex
    mov r0, r6
    mov r1, #0x2000
    SYS 91
    bl puthex
    @ bad arguments: unaligned munmap -> EINVAL; mmap2 len 0 -> EINVAL
    add r0, r6, #1
    mov r1, #0x1000
    SYS 91
    bl puthex
    mov r0, #0
    mov r1, #0
    mov r2, #3
    mov r3, #0x22
    mvn r4, #0
    mov r5, #0
    SYS 192
    bl puthex
    @ cacheflush
    ldr r0, =_start
    add r1, r0, #0x100
    mov r2, #0
    ARMSYS 2                @ (its result is not compared: qemu's differs from the kernel's 0)
    mov r0, #0
    bl exit
