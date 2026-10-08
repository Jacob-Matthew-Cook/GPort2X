@ Processes and signals: getpid, fork + wait4 status, a pipe across fork,
@ rt_sigaction + kill(self) + a handler returning through its restorer,
@ rt_sigprocmask + rt_sigpending, nanosleep, uname, getrlimit, exit_group.
    .include "common.S"
    .text
    .global _start
_start:
    SYS 20
    cmp r0, #0
    movgt r0, #1
    bl puthex               @ 1
    @ pipe
    ldr r0, =pipefds
    SYS 42
    bl puthex
    @ fork
    SYS 2
    cmp r0, #0
    bne parent
    @ child: write to the pipe, exit 7
    ldr r1, =pipefds
    ldr r0, [r1, #4]
    ldr r1, =ping
    mov r2, #4
    SYS 4
    mov r0, #7
    bl exit
parent:
    mov r4, r0
    @ wait4(pid, &status, 0, 0)
    mov r0, r4
    ldr r1, =status
    mov r2, #0
    mov r3, #0
    SYS 114
    cmp r0, r4
    moveq r0, #1
    bl puthex               @ 1: the child's pid
    ldr r1, =status
    ldr r0, [r1]
    bl puthex               @ 700
    @ read the child's 4 bytes
    ldr r1, =pipefds
    ldr r0, [r1]
    ldr r1, =buf
    mov r2, #16
    SYS 3
    bl puthex               @ 4
    ldr r0, =buf
    mov r1, #4
    bl putn
    ldr r0, =nl
    bl puts
    @ wait4 again -> ECHILD
    mvn r0, #0
    ldr r1, =status
    mov r2, #0
    mov r3, #0
    SYS 114
    bl puthex               @ fffffff6
    @ rt_sigaction(SIGUSR1 = 10, &act, 0, 8): handler + SA_RESTORER + restorer
    ldr r1, =act
    ldr r0, =handler
    str r0, [r1]
    ldr r0, =0x04000004     @ SA_RESTORER | SA_SIGINFO (an rt frame, so the restorer's rt_sigreturn matches)
    str r0, [r1, #4]
    ldr r0, =restorer
    str r0, [r1, #8]
    mov r0, #0
    str r0, [r1, #12]
    str r0, [r1, #16]
    mov r0, #10
    mov r2, #0
    mov r3, #8
    SYS 174
    bl puthex               @ 0
    @ kill(getpid(), SIGUSR1): the handler prints "sig" before we print "back"
    SYS 20
    mov r1, #10
    SYS 37
    bl puthex               @ 0
    ldr r0, =back
    bl puts
    @ block SIGUSR1, raise it, check pending, unblock -> the handler runs
    ldr r1, =set
    mov r0, #1 << 9         @ bit (10 - 1)
    str r0, [r1]
    mov r0, #0
    str r0, [r1, #4]
    mov r0, #0              @ SIG_BLOCK
    mov r2, #0
    mov r3, #8
    SYS 175
    bl puthex
    SYS 20
    mov r1, #10
    SYS 37
    ldr r0, =pending
    mov r1, #8
    SYS 176
    ldr r1, =pending
    ldr r0, [r1]
    bl puthex               @ 200
    ldr r0, =unblocking
    bl puts
    mov r0, #1              @ SIG_UNBLOCK
    ldr r1, =set
    mov r2, #0
    mov r3, #8
    SYS 175
    bl puthex
    @ nanosleep 1 ms
    ldr r0, =ts
    mov r1, #0
    SYS 162
    bl puthex
    @ uname -> sysname
    ldr r0, =utsbuf
    SYS 122
    bl puthex
    ldr r0, =utsbuf
    bl puts
    ldr r0, =nl
    bl puts
    @ ugetrlimit(RLIMIT_NOFILE = 7) -> cur
    mov r0, #7
    ldr r1, =rlim
    SYS 191
    bl puthex
    ldr r1, =rlim
    ldr r0, [r1]
    cmp r0, #256
    movge r0, #1            @ the value is the host's under qemu; the device's 1024 here
    bl puthex               @ 1
    @ exit_group(5)
    mov r0, #5
    SYS 248
    b .

handler:
    push {r0-r3, lr}
    ldr r0, =sig
    bl puts
    pop {r0-r3, lr}
    bx lr

restorer:
    SYS 173
    b .

    .data
ping:   .ascii "ping"
nl:     .asciz "\n"
back:   .asciz "back\n"
sig:    .asciz "sig\n"
unblocking: .asciz "unblocking\n"
ts:     .word 0, 1000000
    .bss
    .align 4
pipefds: .space 8
status: .space 4
buf:    .space 64
act:    .space 32
set:    .space 8
pending: .space 8
utsbuf: .space 400
rlim:   .space 8
