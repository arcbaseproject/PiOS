#include <stdarg.h>
#include "ulib.h"

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if (p < q)
        while (n--)
            *p++ = *q++;
    else
        while (n--)
            p[n] = q[n];
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++)
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    for (const unsigned char *p = s; n--; p++)
        if (*p == (unsigned char)c)
            return (void *)p;
    return NULL;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

long atol(const char *s)
{
    long v = 0;
    int neg = *s == '-';
    if (neg || *s == '+')
        s++;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

void puts(const char *s) { write(1, s, strlen(s)); }

void putnum(long v)
{
    char buf[24];
    int i = sizeof buf;
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        buf[--i] = '-';
    write(1, buf + i, sizeof buf - (size_t)i);
}

void eputs(const char *s) { write(2, s, strlen(s)); }

static const char *errstr(long e)
{
    switch (-e) {
    case ENOENT:    return "no such file or directory";
    case EEXIST:    return "file exists";
    case ENOTDIR:   return "not a directory";
    case EISDIR:    return "is a directory";
    case ENOTEMPTY: return "directory not empty";
    case EACCES:    return "permission denied";
    case ESRCH:     return "no such process";
    case EPERM:     return "operation not permitted";
    case EINVAL:    return "invalid argument";
    case ENOSPC:    return "no space left";
    case EIO:       return "I/O error";
    case EXDEV:     return "cross-device move (copy and remove instead)";
    case ENAMETOOLONG: return "file name too long";
    default:        return 0;
    }
}

void err(const char *who, const char *what, long e)
{
    const char *m = errstr(e);
    eputs(who);
    eputs(": ");
    if (what) {
        eputs(what);
        eputs(": ");
    }
    if (m) {
        eputs(m);
    } else {
        char buf[24];
        int i = sizeof buf;
        unsigned long u = e < 0 ? -(unsigned long)e : (unsigned long)e;
        do
            buf[--i] = (char)('0' + u % 10);
        while (u /= 10);
        eputs("error ");
        write(2, buf + i, sizeof buf - (size_t)i);
    }
    eputs("\n");
}

extern char __sigtramp[];
long __rt_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *old, size_t size);

sighandler_t signal(int sig, sighandler_t h)
{
    struct k_sigaction act = { (uint64_t)h, SA_RESTORER, (uint64_t)__sigtramp, 0 }, old;
    long r = __rt_sigaction(sig, &act, &old, 8);
    return r < 0 ? (sighandler_t)r : (sighandler_t)old.handler;
}

// ---- POSIX-style calls built on the Linux *at() / clone / wait4 syscalls ----
long __clone(unsigned long flags, void *stack, void *ptid, void *tls, void *ctid);
long __wait4(int pid, int *status, int options, void *rusage);
long __openat(int dirfd, const char *path, int flags, int mode);
long __fstatat(int dirfd, const char *path, struct stat *st, int flags);
long __dup3(int fd, int nfd, int flags);
long __fcntl(int fd, int cmd, long arg);
long __pipe2(int fds[2], int flags);
long __mkdirat(int dirfd, const char *path, int mode);
long __unlinkat(int dirfd, const char *path, int flags);
long __renameat(int od, const char *o, int nd, const char *n);

int fork(void) { return (int)__clone(SIGCHLD, 0, 0, 0, 0); }
int waitpid(int pid, int *st, int options) { return (int)__wait4(pid, st, options, 0); }
int stat(const char *path, struct stat *st) { return (int)__fstatat(AT_FDCWD, path, st, 0); }
int pipe(int fds[2]) { return (int)__pipe2(fds, 0); }
int mkdir(const char *path, int mode) { return (int)__mkdirat(AT_FDCWD, path, mode); }
int unlink(const char *path) { return (int)__unlinkat(AT_FDCWD, path, 0); }
int rmdir(const char *path) { return (int)__unlinkat(AT_FDCWD, path, AT_REMOVEDIR); }
int rename(const char *a, const char *b) { return (int)__renameat(AT_FDCWD, a, AT_FDCWD, b); }
long __symlinkat(const char *target, int dirfd, const char *path);
long __readlinkat(int dirfd, const char *path, char *buf, size_t size);
int symlink(const char *target, const char *path) { return (int)__symlinkat(target, AT_FDCWD, path); }
long readlink(const char *path, char *buf, size_t size) { return __readlinkat(AT_FDCWD, path, buf, size); }

int open(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    return (int)__openat(AT_FDCWD, path, flags, mode);
}

int dup2(int fd, int nfd)
{
    if (fd == nfd)
        return __fcntl(fd, F_GETFD, 0) < 0 ? -EBADF : nfd;
    return (int)__dup3(fd, nfd, 0);
}

int tcsetpgrp(int pgid) { return ioctl(0, TIOCSPGRP, &pgid); }
int fcntl(int fd, int cmd, long arg) { return (int)__fcntl(fd, cmd, arg); }
long __accept4(int fd, struct sockaddr_in *a, unsigned *len, int flags);
int accept(int fd, struct sockaddr_in *a, unsigned *len) { return (int)__accept4(fd, a, len, 0); }

int ttymode(int mode)
{
    struct ktermios t;
    int r = ioctl(0, TCGETS, &t);
    if (r < 0)
        return r;
    t.c_lflag = mode == TTY_RAW ? t.c_lflag & ~(uint32_t)(ICANON | ECHO | ISIG)
                                : t.c_lflag | ICANON | ECHO | ISIG;
    return ioctl(0, TCSETS, &t);
}

void (*__exit_hook)(void);
char **environ;

_Noreturn void exit(int code)
{
    if (__exit_hook)
        __exit_hook();
    _exit(code);
}
