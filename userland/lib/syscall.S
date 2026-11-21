#include "uapi.h"
.macro SYSCALL name, nr
.global \name
\name:
    mov     x8, #\nr
    svc     #0
    ret
.endm
.text
SYSCALL exit,    SYS_exit
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
