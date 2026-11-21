// Kernel entry. The Pi firmware (arm_64bit=1) loads kernel8.img at 0x80000
// and jumps here on core 0, normally at EL2. Other cores wait in the
// firmware spin table; we still park any non-zero core that gets here.
// The kernel is linked at KERNEL_VA + 0x80000 (TTBR1); this code turns the MMU
// on and jumps there before any C runs.

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

    // Still at physical addresses with the MMU off: adrp/add are PC-relative,
    // so they yield physical addresses here.
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:
    // Boot tables (refined later by mmu_init): L2 of 2 MiB blocks, RAM normal
    // below 0x3F000000, peripherals device; L1[1] = local peripherals. The same
    // L1 serves TTBR0 (identity, for the jump below) and TTBR1 (kernel at
    // KERNEL_VA + pa): for both, L1 index = pa >> 30.
    adrp    x1, kernel_l2
    add     x1, x1, :lo12:kernel_l2
    ldr     x3, =0x0040000000000705     // normal block: UXN | AF | SH inner | attr1 | valid
    ldr     x4, =0x0060000000000401     // device block: UXN | PXN | AF | attr0 | valid
    mov     x2, #0                      // physical address
    mov     x6, #0x3F000000
    mov     x7, #0x40000000
3:  cmp     x2, x6
    csel    x5, x3, x4, lo
    orr     x5, x5, x2
    str     x5, [x1], #8
    add     x2, x2, #0x200000
    cmp     x2, x7
    b.lo    3b
    adrp    x1, kernel_l1
    add     x1, x1, :lo12:kernel_l1
    adrp    x2, kernel_l2
    add     x2, x2, :lo12:kernel_l2
    orr     x2, x2, #3                  // table descriptor
    str     x2, [x1]
    mov     x2, #0x40000000
    orr     x2, x2, x4                  // 1 GiB device block (same attribute bits)
    str     x2, [x1, #8]

    ldr     x2, =0x44ff00               // MAIR: attr0 device nGnRnE (0x00), attr1 normal WB (0xff), attr2 normal NC (0x44)
    msr     mair_el1, x2
    ldr     x2, =0x00000000B5193519     // TCR: T0SZ=T1SZ=25, 4 KiB granules, WB-WA inner shareable walks
    msr     tcr_el1, x2
    msr     ttbr0_el1, x1
    msr     ttbr1_el1, x1
    dsb     ish
    tlbi    vmalle1
    dsb     ish
    isb
    mrs     x2, sctlr_el1
    mov     x3, #0x1005                 // M, C, I
    orr     x2, x2, x3
    msr     sctlr_el1, x2
    isb
    ldr     x1, =high_entry             // link-time (high) address
    br      x1

high_entry:
    ldr     x1, =__stack_top
    mov     sp, x1
    bl      kernel_main
park:
    wfe
    b       park
