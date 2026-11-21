// string.h extras beyond ulib, ctype, locale ("C" only), strerror, assert.
#include "ulib.h"
#include "ctype.h"
#include "locale.h"
#include "stdio.h"

int strcoll(const char *a, const char *b) { return strcmp(a, b); }

char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c)
            r = s;
        if (!*s)
            return (char *)r;
    }
}

char *strstr(const char *h, const char *n)
{
    size_t k = strlen(n);
    for (; *h; h++)
        if (strncmp(h, n, k) == 0)
            return (char *)h;
    return k ? NULL : (char *)h;
}

size_t strspn(const char *s, const char *set)
{
    size_t n = 0;
    while (s[n] && strchr(set, s[n]))
        n++;
    return n;
}

size_t strcspn(const char *s, const char *set)
{
    size_t n = 0;
    while (s[n] && !strchr(set, s[n]))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *set)
{
    s += strcspn(s, set);
    return *s ? (char *)s : NULL;
}

char *strcpy(char *d, const char *s)
{
    memcpy(d, s, strlen(s) + 1);
    return d;
}

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = 0;
    return d;
}

char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}

char *strerror(int e)
{
    static const char *const msg[] = {
        [EPERM] = "Operation not permitted", [ENOENT] = "No such file or directory",
        [ESRCH] = "No such process", [EINTR] = "Interrupted system call",
        [EIO] = "I/O error", [E2BIG] = "Argument list too long",
        [ENOEXEC] = "Exec format error", [EBADF] = "Bad file descriptor",
        [ECHILD] = "No child processes", [EAGAIN] = "Resource temporarily unavailable",
        [ENOMEM] = "Out of memory", [EACCES] = "Permission denied", [EFAULT] = "Bad address",
        [EBUSY] = "Device or resource busy", [EEXIST] = "File exists",
        [EXDEV] = "Cross-device link", [ENOTDIR] = "Not a directory",
        [EISDIR] = "Is a directory", [EINVAL] = "Invalid argument",
        [EMFILE] = "Too many open files", [EFBIG] = "File too large",
        [ENOSPC] = "No space left on device", [ESPIPE] = "Illegal seek",
        [EPIPE] = "Broken pipe", [ERANGE] = "Result out of range",
        [ENAMETOOLONG] = "File name too long", [ENOSYS] = "Function not implemented",
        [ENOTEMPTY] = "Directory not empty",
    };
    static char unknown[24];
    if (e >= 0 && e < (int)(sizeof msg / sizeof *msg) && msg[e])
        return (char *)msg[e];
    snprintf(unknown, sizeof unknown, "Unknown error %d", e);
    return unknown;
}

int isdigit(int c) { return c >= '0' && c <= '9'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isalpha(int c) { return islower(c) || isupper(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f'); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }

char *setlocale(int cat, const char *locale)
{
    (void)cat;
    return !locale || !*locale || strcmp(locale, "C") == 0 ? "C" : NULL;
}

struct lconv *localeconv(void)
{
    static struct lconv c = { "." };
    return &c;
}

void __assert_fail(const char *expr, const char *file, int line)
{
    fprintf(stderr, "assertion failed: %s (%s:%d)\n", expr, file, line);
    exit(134);
}
