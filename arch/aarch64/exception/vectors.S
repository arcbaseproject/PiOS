// EL1 exception vector table. 16 entries of 0x80 bytes, table 2 KiB aligned.
// Frame layout must match struct trap_frame in include/exception.h (272 bytes).
// x21-x23 are used as scratch after being saved.

.macro SAVE
    sub     sp, sp, #272
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    mrs     x23, sp_el0
    stp     x30, x21, [sp, #240]
    stp     x22, x23, [sp, #256]
.endm

.macro RESTORE
    ldp     x22, x23, [sp, #256]
    msr     sp_el0, x23
    ldp     x30, x21, [sp, #240]
    msr     elr_el1, x21
    msr     spsr_el1, x22
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    add     sp, sp, #272
    eret
.endm

// Unhandled vector: save state and report which slot fired.
.macro BAD n
.balign 0x80
    SAVE
    mov     x0, sp
    mov     x1, #\n
    bl      handle_bad
.endm

.section .text
.balign 0x800
.global vectors
vectors:
    BAD 0   // current EL, SP_EL0: sync
    BAD 1   //   irq
    BAD 2   //   fiq
    BAD 3   //   serror
.balign 0x80
    b       el1_sync        // current EL, SP_ELx: sync
.balign 0x80
    b       el1_irq         //   irq
    BAD 6   //   fiq
    BAD 7   //   serror
.balign 0x80
    b       el0_sync        // lower EL AArch64: sync (syscalls, user faults)
.balign 0x80
    b       el0_irq         //   irq
    BAD 10
    BAD 11
    BAD 12  // lower EL AArch32
    BAD 13
    BAD 14
    BAD 15

el1_sync:
    SAVE
    mov     x0, sp
    bl      handle_sync     // does not return yet (faults are fatal in Phase 2)

el1_irq:
    SAVE
    mov     x0, sp
    bl      handle_irq
    b       exception_return

el0_sync:
    SAVE
    mov     x0, sp
    bl      handle_el0_sync
    b       exception_return

el0_irq:
    SAVE
    mov     x0, sp
    bl      handle_irq

// Also the first-run path of new processes: sp points at a prepared trap frame.
.global exception_return
exception_return:
    RESTORE

// New process (fork child / init): cpu_switch lands here with sp = trap frame.
.global ret_from_fork
ret_from_fork:
    bl      schedule_tail
    b       exception_return
