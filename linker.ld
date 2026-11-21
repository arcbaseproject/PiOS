ENTRY(_start)

/* Kernel runs in the TTBR1 half: VA = KERNEL_VA + PA (see include/mm.h).
 * The firmware loads kernel8.img at physical 0x80000 (the LMAs here). */
KERNEL_VA = 0xFFFFFF8000000000;

SECTIONS
{
    . = KERNEL_VA + 0x80000;
    .text : AT(ADDR(.text) - KERNEL_VA) { KEEP(*(.text.boot)) *(.text .text.*) }
    .rodata : AT(ADDR(.rodata) - KERNEL_VA) { *(.rodata .rodata.*) }
    .data : AT(ADDR(.data) - KERNEL_VA) { *(.data .data.*) }
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    . = ALIGN(16);
    . += 0x4000;                /* 16 KiB boot stack */
    __stack_top = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
