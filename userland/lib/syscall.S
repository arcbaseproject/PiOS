#include "uapi.h"
.macro SYSCALL name, nr
.global \name
\name:
    mov     x8, #\nr
    svc     #0
    ret
.endm
.text
SYSCALL _exit,   SYS_exit
SYSCALL fork,    SYS_fork
SYSCALL write,   SYS_write
SYSCALL getpid,  SYS_getpid
SYSCALL execve,  SYS_execve
SYSCALL waitpid, SYS_waitpid
SYSCALL read,    SYS_read
SYSCALL open,    SYS_open
SYSCALL close,   SYS_close
SYSCALL lseek,   SYS_lseek
SYSCALL stat,    SYS_stat
SYSCALL fstat,   SYS_fstat
SYSCALL dup,     SYS_dup
SYSCALL dup2,    SYS_dup2
SYSCALL pipe,    SYS_pipe
SYSCALL chdir,   SYS_chdir
SYSCALL getcwd,  SYS_getcwd
SYSCALL mkdir,   SYS_mkdir
SYSCALL unlink,  SYS_unlink
SYSCALL rmdir,   SYS_rmdir
SYSCALL rename,  SYS_rename
SYSCALL getdents, SYS_getdents
SYSCALL kill,    SYS_kill
SYSCALL setpgid, SYS_setpgid
SYSCALL tcsetpgrp, SYS_tcsetpgrp
SYSCALL ttymode, SYS_ttymode
SYSCALL sigaction, SYS_sigaction

// Signal handlers return here (lr); sp points at the kernel's sigframe.
.global __sigtramp
__sigtramp:
    mov     x8, #SYS_sigreturn
    svc     #0
SYSCALL getprocs, SYS_getprocs
SYSCALL brk,     SYS_brk
SYSCALL nanosleep, SYS_nanosleep
SYSCALL clock_gettime, SYS_clock_gettime
