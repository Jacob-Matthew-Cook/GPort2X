@ Shared helpers for the guest syscall tests (ARMv4T, static, no libc).
@ Assembled twice: EABI (svc 0, number in r7) and with -defsym OABI=1
@ (swi 0x900000+N). The output must be identical under qemu-arm (EABI) and
@ under GPort2X (both). Helpers: puts (r0 = string), puthex (r0 = value,
@ prints 8 hex digits and a newline), putn (r0 = buffer, r1 = length), die.
    .text
    .arm

    .macro SYS nr
    .ifdef OABI
        swi 0x900000 + \nr
    .else
        ldr r7, =\nr
        swi 0
    .endif
    .endm

    .macro ARMSYS n
    .ifdef OABI
        swi 0x9F0000 + \n
    .else
        ldr r7, =0xF0000 + \n
        swi 0
    .endif
    .endm

    .global puts
puts:
    push {r4, r5, lr}
    mov r4, r0
    mov r1, r0
1:  ldrb r2, [r1], #1
    cmp r2, #0
    bne 1b
    sub r2, r1, r4
    sub r2, r2, #1
    mov r1, r4
    mov r0, #1
    SYS 4
    pop {r4, r5, pc}

    .global putn
putn:
    push {r4, lr}
    mov r2, r1
    mov r1, r0
    mov r0, #1
    SYS 4
    pop {r4, pc}

    .global puthex
puthex:
    push {r4, r5, lr}
    ldr r4, =hexbuf
    mov r5, #8
2:  mov r1, r0, lsr #28
    cmp r1, #10
    addlt r1, r1, #'0'
    addge r1, r1, #'a' - 10
    strb r1, [r4], #1
    mov r0, r0, lsl #4
    subs r5, r5, #1
    bne 2b
    mov r1, #'\n'
    strb r1, [r4]
    mov r0, #1
    ldr r1, =hexbuf
    mov r2, #9
    SYS 4
    pop {r4, r5, pc}

    .global exit
exit:
    SYS 1
    b exit

    .bss
hexbuf: .space 12
