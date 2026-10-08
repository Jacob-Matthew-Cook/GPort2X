@ Call capture (--capture): main calls f(&buf, 5) twice; f reads buf[0],
@ stores buf[0] + n to buf[1], writes "ok\n" to stdout and returns buf[1].
@ tests/sys/test_capture.py checks each record: the entry registers, the
@ first-read bytes (buf[0]), the written bytes (buf[1]), the syscall with
@ its result (3) and the return value.
    .text
    .global _start
_start:
    ldr r0, =buf
    mov r1, #5
    bl f
    ldr r0, =buf
    mov r1, #7
    bl f
    mov r0, #0
    mov r7, #1            @ exit
    svc #0

    .global f
f:
    push {r4, r5, r7, lr}
    ldr r2, [r0]
    add r2, r2, r1
    str r2, [r0, #4]
    mov r4, r2
    mov r0, #1
    ldr r1, =msg
    mov r2, #3
    mov r7, #4            @ write
    svc #0
    mov r0, r4
    pop {r4, r5, r7, pc}

    .data
buf:
    .word 0x11223344, 0
msg:
    .ascii "ok\n"
