@ Files: creat/write/lseek/read/fstat64/close, mkdir/chdir/getcwd, rename,
@ access, unlink, rmdir, errors, getdents64, dup2, fcntl, pipe, stat64.
    .include "common.S"
    .text
    .global _start
_start:
    @ creat f1 -> fd
    ldr r0, =n_f1
    ldr r1, =0644
    SYS 8
    mov r4, r0
    bl puthex
    @ write 12 bytes
    mov r0, r4
    ldr r1, =hello
    mov r2, #12
    SYS 4
    bl puthex
    @ lseek(fd, 6, SEEK_SET)
    mov r0, r4
    mov r1, #6
    mov r2, #0
    SYS 19
    bl puthex
    @ read(fd, buf, 100)
    mov r0, r4
    ldr r1, =buf
    mov r2, #100
    SYS 3
    mov r5, r0
    bl puthex
    ldr r0, =buf
    movs r1, r5
    movmi r1, #0        @ a failed read prints nothing (a real kernel writes up to the fault)
    bl putn
    @ fstat64: st_size (+44 OABI, +48 EABI)
    mov r0, r4
    ldr r1, =statbuf
    SYS 197
    bl puthex
    ldr r1, =statbuf
    .ifdef OABI
    ldr r0, [r1, #44]
    .else
    ldr r0, [r1, #48]
    .endif
    bl puthex
    @ st_mode & S_IFMT
    ldr r1, =statbuf
    ldr r0, [r1, #16]
    and r0, r0, #0xF000
    bl puthex
    @ close
    mov r0, r4
    SYS 6
    bl puthex
    @ mkdir d; chdir d; getcwd -> last char; chdir ..
    ldr r0, =n_d
    ldr r1, =0755
    SYS 39
    bl puthex
    ldr r0, =n_d
    SYS 12
    bl puthex
    ldr r0, =buf
    ldr r1, =4096
    SYS 183
    mov r5, r0
    ldr r0, =buf
    add r0, r0, r5
    sub r0, r0, #2      @ the char before the NUL
    mov r1, #1
    bl putn
    ldr r0, =nl
    bl puts
    ldr r0, =n_dotdot
    SYS 12
    bl puthex
    @ rename f1 -> f2; access f2 F_OK; access f1 -> ENOENT
    ldr r0, =n_f1
    ldr r1, =n_f2
    SYS 38
    bl puthex
    ldr r0, =n_f2
    mov r1, #0
    SYS 33
    bl puthex
    ldr r0, =n_f1
    mov r1, #0
    SYS 33
    bl puthex
    @ stat64 "." -> S_IFDIR
    ldr r0, =n_dot
    ldr r1, =statbuf
    SYS 195
    bl puthex
    ldr r1, =statbuf
    ldr r0, [r1, #16]
    and r0, r0, #0xF000
    bl puthex
    @ open in a missing directory -> ENOENT; open O_CREAT|O_EXCL twice -> EEXIST
    ldr r0, =n_nodir
    ldr r1, =0101
    ldr r2, =0644
    SYS 5
    bl puthex
    ldr r0, =n_f2
    ldr r1, =0301
    ldr r2, =0644
    SYS 5
    bl puthex
    @ getdents64 of "." after creating a, b: print names except . and ..
    ldr r0, =n_a
    ldr r1, =0644
    SYS 8
    mov r4, r0
    @ dup2(fd, 20) then write through 20, fcntl F_GETFD/F_SETFD
    mov r0, r4
    mov r1, #20
    SYS 63
    bl puthex
    mov r0, #20
    ldr r1, =hello
    mov r2, #5
    SYS 4
    bl puthex
    mov r0, #20
    mov r1, #2
    mov r2, #1
    SYS 221
    bl puthex
    mov r0, #20
    mov r1, #1
    mov r2, #0
    SYS 221
    bl puthex
    mov r0, #20
    mov r1, #3
    mov r2, #0
    SYS 221
    and r0, r0, #3
    bl puthex
    mov r0, r4
    SYS 6
    mov r0, #20
    SYS 6
    bl puthex
    mov r0, #20
    SYS 6
    bl puthex              @ EBADF
    ldr r0, =n_b
    ldr r1, =0644
    SYS 8
    SYS 6
    ldr r0, =n_dot
    ldr r1, =0200000 | 040000   @ O_DIRECTORY (EABI value; see below)
    .ifdef OABI
    ldr r1, =040000
    .else
    ldr r1, =040000
    .endif
    mov r2, #0
    SYS 5
    mov r4, r0
    @ loop: getdents64(fd, buf, 4096)
3:  mov r0, r4
    ldr r1, =buf
    ldr r2, =4096
    SYS 217
    cmp r0, #0
    ble 5f
    mov r5, r0            @ bytes
    ldr r6, =buf
4:  ldrh r1, [r6, #16]    @ d_reclen
    add r0, r6, #19       @ d_name
    @ skip "." and ".."
    ldrb r2, [r0]
    cmp r2, #'.'
    bne 6f
    ldrb r2, [r0, #1]
    cmp r2, #0
    beq 7f
    cmp r2, #'.'
    bne 6f
    ldrb r2, [r0, #2]
    cmp r2, #0
    beq 7f
6:  push {r1}
    bl puts
    ldr r0, =nl
    bl puts
    pop {r1}
7:  add r6, r6, r1
    subs r5, r5, r1
    bgt 4b
    b 3b
5:  bl puthex             @ 0 at the end
    mov r0, r4
    SYS 6
    @ unlink a, b, f2; rmdir d; unlink missing -> ENOENT; rmdir file -> ENOTDIR
    ldr r0, =n_a
    SYS 10
    bl puthex
    ldr r0, =n_b
    SYS 10
    bl puthex
    ldr r0, =n_f2
    SYS 10
    bl puthex
    ldr r0, =n_d
    SYS 40
    bl puthex
    ldr r0, =n_a
    SYS 10
    bl puthex
    ldr r0, =n_dot
    SYS 10
    bl puthex              @ EISDIR
    @ pipe: write 5, read 5
    ldr r0, =pipefds
    SYS 42
    bl puthex
    ldr r1, =pipefds
    ldr r0, [r1, #4]
    ldr r1, =hello
    mov r2, #5
    SYS 4
    bl puthex
    ldr r1, =pipefds
    ldr r0, [r1]
    ldr r1, =buf
    mov r2, #100
    SYS 3
    bl puthex
    ldr r0, =buf
    mov r1, #5
    bl putn
    ldr r0, =nl
    bl puts
    mov r0, #0
    bl exit

    .data
n_f1:   .asciz "f1"
n_f2:   .asciz "f2"
n_d:    .asciz "d"
n_dot:  .asciz "."
n_dotdot: .asciz ".."
n_nodir: .asciz "nodir/x"
n_a:    .asciz "a"
n_b:    .asciz "b"
hello:  .ascii "hello world\n"
nl:     .asciz "\n"
    .bss
    .align 4
buf:    .space 4096
statbuf: .space 128
pipefds: .space 8
