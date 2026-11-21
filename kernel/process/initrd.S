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
    FILE /bin, fpu
    FILE /bin, sh
    FILE /bin, ls
    FILE /bin, pwd
    FILE /bin, cat
    FILE /bin, echo
    FILE /bin, mkdir
    FILE /bin, rm
    FILE /bin, cp
    FILE /bin, mv
    FILE /bin, ps
    FILE /bin, kill
    FILE /bin, uname
    FILE /bin, clear
    FILE /bin, sleep
    FILE /bin, uptime
    FILE /bin, ed
    FILE /bin, edit
    .section .rodata.initrd_table
    .quad 0, 0, 0
