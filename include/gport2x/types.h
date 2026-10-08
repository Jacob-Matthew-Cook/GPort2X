/* GPort2X: shared guest types, layout constants and error codes.
 * Section numbers ("spec N") refer to the harness specification. */
#ifndef GPORT2X_TYPES_H
#define GPORT2X_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t gaddr_t;  /* a guest virtual address (the guest is 32-bit) */
typedef uint32_t gpaddr_t; /* a GP2X physical address, as reached via /dev/mem */

#define GP2X_PAGE_SHIFT 12u
#define GP2X_PAGE_SIZE (1u << GP2X_PAGE_SHIFT)
#define GP2X_PAGE_MASK (~(GP2X_PAGE_SIZE - 1u))
#define GP2X_PAGE_ALIGN_UP(x) (((x) + GP2X_PAGE_SIZE - 1u) & GP2X_PAGE_MASK)

/* Guest virtual layout: Linux 2.4 on the GP2X (spec 1.4). */
#define GUEST_LOAD_BASE 0x00008000u /* first PT_LOAD of every guest ELF */
#define GUEST_MMAP_BASE 0x40000000u /* TASK_UNMAPPED_BASE: mmap grows up from here */
#define GUEST_TASK_SIZE 0xC0000000u /* the initial stack sits just below this */
#define GUEST_STACK_TOP GUEST_TASK_SIZE

/* GP2X physical model: what /dev/mem offsets may reach (spec 1.5, 3.2). */
#define GP2X_UPPER_BANK_PHYS 0x02000000u
#define GP2X_UPPER_BANK_SIZE 0x02000000u /* 32 MB: framebuffers, video windows, 940 RAM */
#define GP2X_REGS_PHYS 0xC0000000u
#define GP2X_REGS_SIZE 0x00010000u /* 64 KB MMSP2 register file */

/* Harness-internal results: 0 on success, negative on failure.
 * (Guest-visible errno values are produced by the sys module.) */
enum {
    GP_OK = 0,
    GP_ERR_FAULT = -1,       /* guest address unmapped or protection violated */
    GP_ERR_INVAL = -2,       /* bad argument */
    GP_ERR_NOMEM = -3,       /* host allocation failed or no free guest range */
    GP_ERR_EXIST = -4,       /* already present */
    GP_ERR_UNSUPPORTED = -5, /* not implemented in this milestone */
    GP_ERR_IO = -6,          /* host I/O failure */
    GP_ERR_FORMAT = -7,      /* malformed input (ELF, FAT image, ...) */
};

#endif /* GPORT2X_TYPES_H */
