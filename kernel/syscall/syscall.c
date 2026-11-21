// Syscall dispatch. ABI in include/uapi/uapi.h and DESIGN.md.
// shortcut: syscalls run with IRQs masked (exception entry state); fine while all are short or block via schedule().
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "sched.h"
#include "uapi.h"
#include "timer.h"
#include "vfs.h"

long syscall_dispatch(struct trap_frame *tf)
{
    uint64_t *a = tf->x;
    switch (tf->x[8]) {
    case SYS_exit:    do_exit(W_EXITCODE((int)a[0]));
    case SYS_fork:    return sys_fork(tf);
    case SYS_write:   return sys_write((int)a[0], a[1], a[2]);
    case SYS_read:    return sys_read((int)a[0], a[1], a[2]);
    case SYS_open:    return sys_open(a[0], (int)a[1], (int)a[2]);
    case SYS_close:   return sys_close((int)a[0]);
    case SYS_lseek:   return sys_lseek((int)a[0], (int64_t)a[1], (int)a[2]);
    case SYS_stat:    return sys_stat(a[0], a[1]);
    case SYS_fstat:   return sys_fstat((int)a[0], a[1]);
    case SYS_dup:     return sys_dup((int)a[0]);
    case SYS_dup2:    return sys_dup2((int)a[0], (int)a[1]);
    case SYS_pipe:    return sys_pipe(a[0]);
    case SYS_chdir:   return sys_chdir(a[0]);
    case SYS_getcwd:  return sys_getcwd(a[0], a[1]);
    case SYS_mkdir:   return sys_mkdir(a[0], (int)a[1]);
    case SYS_unlink:  return sys_unlink(a[0]);
    case SYS_rmdir:   return sys_rmdir(a[0]);
    case SYS_rename:  return sys_rename(a[0], a[1]);
    case SYS_getdents: return sys_getdents((int)a[0], a[1], a[2]);
    case SYS_kill:    return sys_kill((int)a[0], (int)a[1]);
    case SYS_setpgid: return sys_setpgid((int)a[0], (int)a[1]);
    case SYS_tcsetpgrp: return sys_tcsetpgrp((int)a[0]);
    case SYS_getprocs: return sys_getprocs(a[0], (int)a[1]);
    case SYS_brk:     return sys_brk(a[0]);
    case SYS_nanosleep: return sys_nanosleep(a[0]);
    case SYS_clock_gettime: return sys_clock_gettime((int)a[0], a[1]);
    case SYS_ttymode: return sys_ttymode((int)a[0]);
    case SYS_getpid:  return current_task()->pid;
    case SYS_execve:  return sys_execve(tf, a[0], a[1], a[2]);
    case SYS_waitpid: return sys_waitpid((int)a[0], a[1], (int)a[2]);
    default:          return -ENOSYS;
    }
}
