/* Kernel/user ABI shared by kernel and userland. Plain #defines only (used from .S). */
#pragma once

#define PIOS_VERSION "0.6"

/* Syscall ABI (AArch64): number in x8, args in x0-x5, `svc #0`,
 * result in x0. Negative result = -errno. All other registers preserved. */
#define SYS_exit    1
#define SYS_fork    2
#define SYS_write   3
#define SYS_getpid  4
#define SYS_execve  5
#define SYS_waitpid 6
#define SYS_read    7
#define SYS_open    8
#define SYS_close   9
#define SYS_lseek   10
#define SYS_stat    11
#define SYS_fstat   12
#define SYS_dup     13
#define SYS_dup2    14
#define SYS_pipe    15
#define SYS_chdir   16
#define SYS_getcwd  17
#define SYS_mkdir   18
#define SYS_unlink  19
#define SYS_rmdir   20
#define SYS_rename  21
#define SYS_getdents 22
#define SYS_kill     23     /* kill(pid, sig): any sig 1..31 terminates; no handlers */
#define SYS_setpgid  24     /* setpgid(pid, pgid): self or own child; 0 = self / own pid */
#define SYS_tcsetpgrp 25    /* tcsetpgrp(pgid): console ^C target; 0 = none (^C discards line) */
#define SYS_getprocs 26     /* getprocs(struct procinfo *buf, int max) -> count */
#define SYS_brk      27     /* brk(addr) -> new break; old break on failure; brk(0) queries */
#define SYS_nanosleep 28    /* nanosleep(const struct timespec *req, NULL); 10 ms resolution */
#define SYS_clock_gettime 29 /* clock_gettime(CLOCK_MONOTONIC, struct timespec *): time since boot */
#define CLOCK_MONOTONIC 1
#define SYS_ttymode  30     /* ttymode(TTY_RAW|TTY_COOKED): console line discipline; raw passes every byte (incl. ^C), no echo */
#define TTY_COOKED 0
#define TTY_RAW    1

/* errno values (Linux-compatible numbering) */
#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define E2BIG   7
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EFAULT  14
#define EINVAL  22
#define ENAMETOOLONG 36
#define ENOSYS  38
#define EACCES  13
#define EBUSY   16
#define EEXIST  17
#define EXDEV   18
#define ENOTDIR 20
#define EISDIR  21
#define EMFILE  24
#define EFBIG   27
#define ENOSPC  28
#define ESPIPE  29
#define EPIPE   32
#define ERANGE  34
#define ENOTEMPTY 39

/* waitpid status encoding (POSIX-style) */
#define W_EXITCODE(code) (((code) & 0xff) << 8)
#define SIGINT  2
#define SIGILL  4
#define SIGKILL 9
#define SIGSEGV 11
#define SIGTERM 15

/* open() flags (Linux numbering) */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_ACCMODE 3
#define O_CREAT   0x40
#define O_EXCL    0x80
#define O_TRUNC   0x200
#define O_APPEND  0x400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* st_mode file types */
#define S_IFMT   0170000
#define S_IFIFO  0010000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

#define NAME_MAX 55
#define PATH_MAX 256

#ifndef __ASSEMBLER__
#include <stdint.h>
struct stat {
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_pad;
    uint64_t st_size;
};

/* getdents() record: fixed 64 bytes */
struct dirent {
    uint32_t d_ino;
    uint32_t d_mode;        /* file type bits of st_mode */
    char d_name[NAME_MAX + 1];
};

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* getprocs() record */
struct procinfo {
    int32_t pid, ppid, pgid;
    char state;             /* R running/runnable, S waiting, Z zombie */
    char name[15];
};
#endif
