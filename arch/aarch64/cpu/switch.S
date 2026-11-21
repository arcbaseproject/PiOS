// void cpu_switch(struct cpu_context *prev, struct cpu_context *next)
// Saves callee-saved regs, fp, lr, sp of prev; loads next's; returns into next.
.global cpu_switch
cpu_switch:
    mov     x9, sp
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    str     x9,       [x0, #96]
    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]
    ldr     x9,       [x1, #96]
    mov     sp, x9
    ret

// First run of a new kernel thread: x19 = fn, x20 = arg (set by kthread_create).
// Arrives with IRQs masked (switched from schedule()), so unmask here.
.global task_entry
task_entry:
    bl      schedule_tail
    msr     daifclr, #2
    mov     x0, x20
    blr     x19
    bl      task_exit

// void fpsimd_save(struct fpstate *), fpsimd_load(const struct fpstate *)
// q0-q31 then fpsr, fpcr. Layout must match struct fpstate in sched.h.
.macro QREGS op
    \op    q0, q1, [x0, #0]
    \op    q2, q3, [x0, #32]
    \op    q4, q5, [x0, #64]
    \op    q6, q7, [x0, #96]
    \op    q8, q9, [x0, #128]
    \op    q10, q11, [x0, #160]
    \op    q12, q13, [x0, #192]
    \op    q14, q15, [x0, #224]
    \op    q16, q17, [x0, #256]
    \op    q18, q19, [x0, #288]
    \op    q20, q21, [x0, #320]
    \op    q22, q23, [x0, #352]
    \op    q24, q25, [x0, #384]
    \op    q26, q27, [x0, #416]
    \op    q28, q29, [x0, #448]
    \op    q30, q31, [x0, #480]
.endm
.global fpsimd_save
fpsimd_save:
    QREGS stp
    mrs     x9, fpsr
    mrs     x10, fpcr
    add     x0, x0, #512
    stp     x9, x10, [x0]
    ret

.global fpsimd_load
fpsimd_load:
    QREGS ldp
    add     x0, x0, #512
    ldp     x9, x10, [x0]
    msr     fpsr, x9
    msr     fpcr, x10
    ret
