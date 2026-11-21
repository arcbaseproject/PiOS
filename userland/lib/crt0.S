// Kernel enters with x0=argc, x1=argv, x2=envp, sp 16-byte aligned.
.section .text.start
.global _start
_start:
    bl      main
    bl      exit
