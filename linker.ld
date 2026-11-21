ENTRY(_start)

SECTIONS
{
    . = 0x80000;                /* firmware load address for kernel8.img */
    .text : { KEEP(*(.text.boot)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
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
