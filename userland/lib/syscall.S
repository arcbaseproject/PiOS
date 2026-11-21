#include "uapi.h"
.macro SYSCALL name, nr
.global \name
\name:
    mov     x8, #\nr
    svc     #0
    ret
.endm
.text
// Direct Linux syscalls (same arguments as the C prototypes in ulib.h).
SYSCALL _exit,   SYS_exit_group
SYSCALL write,   SYS_write
SYSCALL read,    SYS_read
SYSCALL getpid,  SYS_getpid
SYSCALL execve,  SYS_execve
SYSCALL close,   SYS_close
SYSCALL lseek,   SYS_lseek
SYSCALL fstat,   SYS_fstat
SYSCALL dup,     SYS_dup
SYSCALL chdir,   SYS_chdir
SYSCALL getcwd,  SYS_getcwd
SYSCALL getdents64, SYS_getdents64
SYSCALL kill,    SYS_kill
SYSCALL setpgid, SYS_setpgid
SYSCALL getprocs, SYS_getprocs
SYSCALL brk,     SYS_brk
SYSCALL nanosleep, SYS_nanosleep
SYSCALL clock_gettime, SYS_clock_gettime
SYSCALL ioctl,   SYS_ioctl
SYSCALL mmap,    SYS_mmap
SYSCALL munmap,  SYS_munmap
SYSCALL uname,   SYS_uname
SYSCALL socket,  SYS_socket
SYSCALL bind,    SYS_bind
SYSCALL listen,  SYS_listen
SYSCALL __accept4, SYS_accept4
SYSCALL connect, SYS_connect
SYSCALL sendto,  SYS_sendto
SYSCALL recvfrom, SYS_recvfrom
// Raw forms behind C wrappers in ulib.c.
SYSCALL __clone, SYS_clone
SYSCALL __wait4, SYS_wait4
SYSCALL __openat, SYS_openat
SYSCALL __fstatat, SYS_newfstatat
SYSCALL __dup3,  SYS_dup3
SYSCALL __fcntl, SYS_fcntl
SYSCALL __pipe2, SYS_pipe2
SYSCALL __mkdirat, SYS_mkdirat
SYSCALL __unlinkat, SYS_unlinkat
SYSCALL __renameat, SYS_renameat
SYSCALL __rt_sigaction, SYS_rt_sigaction
SYSCALL __symlinkat, SYS_symlinkat
SYSCALL __readlinkat, SYS_readlinkat

// Signal handlers return here (lr); sp points at the kernel's sigframe.
.global __sigtramp
__sigtramp:
    mov     x8, #SYS_rt_sigreturn
    svc     #0
