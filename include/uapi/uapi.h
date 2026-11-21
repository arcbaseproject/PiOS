/* Kernel/user ABI shared by kernel and userland. Plain #defines only (used from .S). */
#pragma once

#define PIOS_VERSION "0.7"

/* Syscall ABI: Linux/AArch64. Number in x8, args in x0-x5, `svc #0`, result
 * in x0, negative = -errno. Numbers and structures match Linux (asm-generic
 * unistd), so static Linux binaries run; PiOS-only calls are >= 1000. */
#define SYS_getcwd        17
#define SYS_dup           23
#define SYS_dup3          24
#define SYS_fcntl         25
#define SYS_ioctl         29
#define SYS_mkdirat       34
#define SYS_symlinkat     36
#define SYS_unlinkat      35
#define SYS_renameat      38
#define SYS_faccessat     48
#define SYS_chdir         49
#define SYS_openat        56
#define SYS_close         57
#define SYS_pipe2         59
#define SYS_getdents64    61
#define SYS_ppoll         73
#define SYS_lseek         62
#define SYS_read          63
#define SYS_write         64
#define SYS_readv         65
#define SYS_writev        66
#define SYS_readlinkat    78
#define SYS_newfstatat    79
#define SYS_fstat         80
#define SYS_fsync         82
#define SYS_exit          93
#define SYS_exit_group    94
#define SYS_set_tid_address 96
#define SYS_nanosleep     101
#define SYS_clock_gettime 113
#define SYS_sched_yield   124
#define SYS_kill          129
#define SYS_tkill         130
#define SYS_tgkill        131
#define SYS_rt_sigaction  134
#define SYS_rt_sigprocmask 135
#define SYS_rt_sigreturn  139
#define SYS_setpgid       154
#define SYS_getpgid       155
#define SYS_getsid        156
#define SYS_setsid        157
#define SYS_uname         160
#define SYS_umask         166
#define SYS_getpid        172
#define SYS_getppid       173
#define SYS_getuid        174
#define SYS_geteuid       175
#define SYS_getgid        176
#define SYS_getegid       177
#define SYS_gettid        178
#define SYS_brk           214
#define SYS_munmap        215
#define SYS_clone         220     /* only fork-like: flags SIGCHLD, or CLONE_VM|CLONE_VFORK|SIGCHLD (copied) */
#define SYS_execve        221
#define SYS_mmap          222     /* anonymous private mappings only */
#define SYS_mprotect      226
#define SYS_madvise       233
#define SYS_wait4         260
#define SYS_getprocs      1000    /* PiOS: getprocs(struct procinfo *buf, int max) -> count */

#define AT_FDCWD        (-100)
#define AT_REMOVEDIR    0x200
#define AT_SYMLINK_NOFOLLOW 0x100
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLONE_VM        0x100
#define CLONE_VFORK     0x4000
#define WNOHANG         1

/* ioctl on terminals (the console) */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413
#define ISIG    0x1         /* c_lflag */
#define ICANON  0x2
#define ECHO    0x8

/* fcntl */
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

/* mmap */
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* ppoll */
#define POLLIN   0x01
#define POLLOUT  0x04
#define POLLERR  0x08
#define POLLHUP  0x10
#define POLLNVAL 0x20

/* rt_sigaction */
#define SIG_DFL 0
#define SIG_IGN 1
#define SA_SIGINFO  0x4
#define SA_RESTORER 0x04000000
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

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
#define ENOTTY  25
#define ELOOP   40
#define ENODEV  19

/* wait status encoding (POSIX-style) */
#define W_EXITCODE(code) (((code) & 0xff) << 8)
#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGILL  4
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG  23
#define SIGWINCH 28

/* open() flags (Linux numbering) */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_ACCMODE 3
#define O_CREAT   0x40
#define O_EXCL    0x80
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define O_NONBLOCK 0x800
#define O_DIRECTORY 0x4000    /* arm64 value */
#define O_CLOEXEC 0x80000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* st_mode file types */
#define S_IFMT   0170000
#define S_IFIFO  0010000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define DT_FIFO 1           /* dirent64.d_type */
#define DT_CHR  2
#define DT_DIR  4
#define DT_REG  8
#define DT_LNK  10
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

#define NAME_MAX 55
#define PATH_MAX 256

#ifndef __ASSEMBLER__
#include <stdint.h>
struct stat {               /* Linux asm-generic, 128 bytes */
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid, st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t st_size;
    int32_t st_blksize, __pad2;
    int64_t st_blocks;
    int64_t st_atime_sec, st_atime_nsec;
    int64_t st_mtime_sec, st_mtime_nsec;
    int64_t st_ctime_sec, st_ctime_nsec;
    uint32_t __unused[2];
};

struct dirent64 {           /* getdents64() record, 8-byte aligned, variable length */
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

struct ktermios {           /* TCGETS/TCSETS argument (kernel layout, 36 bytes used) */
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line;
    uint8_t c_cc[19];
};

struct winsize {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
};

struct k_sigaction {        /* rt_sigaction argument */
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct pollfd {
    int32_t fd;
    int16_t events, revents;
};

struct iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

struct utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
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
