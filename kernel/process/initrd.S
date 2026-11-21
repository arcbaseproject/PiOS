// Table of {path, start, end} for user ELFs built into build/user/.
// vfs_init() copies them into ramfs; execve then loads them from there.
.macro FILE dir, name
    .section .rodata.initrd_names
1:  .asciz "\dir/\name"
    .section .rodata.initrd_data
    .balign 8
2:  .incbin "build/user/\name\().elf"
3:
    .section .rodata.initrd_table
    .quad 1b, 2b, 3b
.endm

.section .rodata.initrd_table
.balign 8
.global initrd_table
initrd_table:
    FILE /sbin, init
    FILE /bin, hello
    FILE /bin, segv
    FILE /bin, spin
    .section .rodata.initrd_table
    .quad 0, 0, 0
