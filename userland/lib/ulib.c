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

sighandler_t signal(int sig, sighandler_t h)
{
    return (sighandler_t)sigaction(sig, (void *)h, __sigtramp);
}

void (*__exit_hook)(void);
char **environ;

_Noreturn void exit(int code)
{
    if (__exit_hook)
        __exit_hook();
    _exit(code);
}
