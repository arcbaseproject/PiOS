// Kernel enters with x0=argc, x1=argv, x2=envp, sp 16-byte aligned.
.section .text.start
.global _start
_start:
    adrp    x9, environ
    str     x2, [x9, :lo12:environ]
    bl      main
    bl      exit
