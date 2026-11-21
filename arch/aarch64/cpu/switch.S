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
