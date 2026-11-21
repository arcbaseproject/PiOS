// Kernel entry. The Pi firmware (arm_64bit=1) loads kernel8.img at 0x80000
// and jumps here on core 0, normally at EL2. Other cores wait in the
// firmware spin table; we still park any non-zero core that gets here.

.section .text.boot
.global _start
_start:
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff           // Aff0 = core number
    cbnz    x1, park

    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.eq    from_el2
    cmp     x1, #1
    b.eq    el1_entry
    b       park                    // EL3 not supported (firmware never gives it with default armstub)

from_el2:
    mov     x1, #(1 << 31)          // HCR_EL2.RW: EL1 runs AArch64
    msr     hcr_el2, x1
    mov     x1, #3                  // CNTHCTL_EL2.EL1PCTEN|EL1PCEN: EL1 may use physical timer
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr
    mov     x1, #0x33ff             // CPTR_EL2: RES1 bits, TFP=0: don't trap FP/SIMD to EL2
    msr     cptr_el2, x1
    ldr     x1, =0x30d00800         // SCTLR_EL1: RES1 bits only, MMU and caches off
    msr     sctlr_el1, x1
    mov     x1, #0x3c5              // SPSR: EL1h, DAIF masked
    msr     spsr_el2, x1
    adr     x1, el1_entry
    msr     elr_el2, x1
    eret

el1_entry:
    mov     x1, #(3 << 20)          // CPACR_EL1.FPEN: FP/SIMD usable at EL0/EL1 (kernel C never uses it)
    msr     cpacr_el1, x1
    isb
    ldr     x1, =__stack_top
    mov     sp, x1

    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:
    bl      kernel_main
park:
    wfe
    b       park
