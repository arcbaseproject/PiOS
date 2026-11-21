// Syscall dispatch. ABI in include/uapi/uapi.h and DESIGN.md.
// shortcut: syscalls run with IRQs masked (exception entry state); fine while all are short or block via schedule().
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "sched.h"
#include "uapi.h"
#include "timer.h"
#include "vfs.h"

static long dispatch(struct trap_frame *tf);

// Build with `make STRACE=1` to log syscalls that fail with errors that usually
// mean "PiOS lacks this" (handy when porting a Linux binary).
long syscall_dispatch(struct trap_frame *tf)
{
#ifdef STRACE
    uint64_t nr = tf->x[8], a0 = tf->x[0], a1 = tf->x[1], a2 = tf->x[2];
    long r = dispatch(tf);
    if (r == -EINVAL || r == -ENOSYS || r == -ENOTTY || r == -EFAULT)
        kprintf("[STRACE] %s: nr %lu (%lx, %lx, %lx) = %ld\n", current_task()->name, nr, a0, a1, a2, r);
    return r;
#else
    return dispatch(tf);
#endif
}

static long dispatch(struct trap_frame *tf)
{
    uint64_t *a = tf->x;
    switch (tf->x[8]) {
    case SYS_exit:
    case SYS_exit_group: do_exit(W_EXITCODE((int)a[0]));
    case SYS_clone:   return sys_clone(tf, a[0], a[1]);
    case SYS_execve:  return sys_execve(tf, a[0], a[1], a[2]);
    case SYS_wait4:   return sys_wait4((int)a[0], a[1], (int)a[2]);
    case SYS_read:    return sys_read((int)a[0], a[1], a[2]);
    case SYS_write:   return sys_write((int)a[0], a[1], a[2]);
    case SYS_readv:   return sys_rwv((int)a[0], a[1], (int)a[2], 0);
    case SYS_writev:  return sys_rwv((int)a[0], a[1], (int)a[2], 1);
    case SYS_openat:  return sys_openat((int)a[0], a[1], (int)a[2], (int)a[3]);
    case SYS_close:   return sys_close((int)a[0]);
    case SYS_lseek:   return sys_lseek((int)a[0], (int64_t)a[1], (int)a[2]);
    case SYS_newfstatat: return sys_newfstatat((int)a[0], a[1], a[2], (int)a[3]);
    case SYS_fstat:   return sys_fstat((int)a[0], a[1]);
    case SYS_dup:     return sys_dup((int)a[0]);
    case SYS_dup3:    return sys_dup3((int)a[0], (int)a[1], (int)a[2]);
    case SYS_fcntl:   return sys_fcntl((int)a[0], (int)a[1], a[2]);
    case SYS_ioctl:   return sys_ioctl((int)a[0], a[1], a[2]);
    case SYS_pipe2:   return sys_pipe2(a[0], (int)a[1]);
    case SYS_chdir:   return sys_chdir(a[0]);
    case SYS_getcwd:  return sys_getcwd(a[0], a[1]);
    case SYS_mkdirat: return sys_mkdirat((int)a[0], a[1], (int)a[2]);
    case SYS_unlinkat: return sys_unlinkat((int)a[0], a[1], (int)a[2]);
    case SYS_renameat: return sys_renameat((int)a[0], a[1], (int)a[2], a[3]);
    case SYS_faccessat: return sys_faccessat((int)a[0], a[1], (int)a[2]);
    case SYS_readlinkat: return sys_readlinkat((int)a[0], a[1], a[2], a[3]);
    case SYS_symlinkat: return sys_symlinkat(a[0], (int)a[1], a[2]);
    case SYS_ppoll:   return sys_ppoll(a[0], a[1], a[2]);
    case SYS_getdents64: return sys_getdents64((int)a[0], a[1], a[2]);
    case SYS_fsync:   return 0;                    // FAT writes are write-through
    case SYS_umask:   return sys_umask((int)a[0]);
    case SYS_kill:    return sys_kill((int)a[0], (int)a[1]);
    case SYS_tkill:   return sys_kill((int)a[0], (int)a[1]);
    case SYS_tgkill:  return sys_kill((int)a[1], (int)a[2]);
    case SYS_rt_sigaction: return sys_rt_sigaction((int)a[0], a[1], a[2], a[3]);
    case SYS_rt_sigprocmask: return sys_rt_sigprocmask((int)a[0], a[1], a[2], a[3]);
    case SYS_rt_sigreturn: return sys_rt_sigreturn(tf);
    case SYS_setpgid: return sys_setpgid((int)a[0], (int)a[1]);
    case SYS_getpgid: return sys_getpgid((int)a[0]);
    case SYS_getsid:  return sys_getpgid((int)a[0]);
    case SYS_setsid:  return sys_setsid();
    case SYS_getpid:
    case SYS_gettid:
    case SYS_set_tid_address: return current_task()->pid;
    case SYS_getppid: return sys_getppid();
    case SYS_getuid: case SYS_geteuid: case SYS_getgid: case SYS_getegid: return 0;
    case SYS_uname:   return sys_uname(a[0]);
    case SYS_sched_yield: schedule(); return 0;
    case SYS_brk:     return sys_brk(a[0]);
    case SYS_mmap:    return sys_mmap(a[0], a[1], (int)a[2], (int)a[3], (int)a[4], (int64_t)a[5]);
    case SYS_munmap:  return sys_munmap(a[0], a[1]);
    case SYS_mprotect: case SYS_madvise: return 0;
    case SYS_nanosleep: return sys_nanosleep(a[0]);
    case SYS_clock_gettime: return sys_clock_gettime((int)a[0], a[1]);
    case SYS_getprocs: return sys_getprocs(a[0], (int)a[1]);
    default:          return -ENOSYS;
    }
}
