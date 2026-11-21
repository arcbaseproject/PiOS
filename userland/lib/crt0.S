// Linux process entry: sp -> argc, argv[0..argc-1], NULL, envp..., NULL, auxv.
.section .text.start
.global _start
_start:
    ldr     x0, [sp]                // argc
    add     x1, sp, #8              // argv
    add     x2, x1, x0, lsl #3
    add     x2, x2, #8              // envp = argv + argc + 1
    adrp    x9, environ
    str     x2, [x9, :lo12:environ]
    bl      main
    bl      exit
