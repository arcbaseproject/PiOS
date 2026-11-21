// printf family: %d %i %u %x %X %p %s %c %%, flags - 0, width, .precision
// (strings), length l ll z. No floating point.
#include <stdarg.h>
#include "ulib.h"

struct out {
    char *buf;
    size_t cap, len;
};

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->cap)
        o->buf[o->len] = c;
    o->len++;
}

static void pad(struct out *o, char c, int n)
{
    while (n-- > 0)
        put(o, c);
}

int vsnprintf(char *buf, size_t cap, const char *f, va_list ap)
{
    struct out o = { buf, cap, 0 };
    for (; *f; f++) {
        if (*f != '%') {
            put(&o, *f);
            continue;
        }
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
        for (;; f++) {
            if (f[1] == '-')
                left = 1;
            else if (f[1] == '0')
                zero = 1;
            else
                break;
        }
        f++;
        if (*f == '*') {
            width = va_arg(ap, int);
            f++;
        } else {
            while (*f >= '0' && *f <= '9')
                width = width * 10 + (*f++ - '0');
        }
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') {
                prec = va_arg(ap, int);
                f++;
            } else {
                while (*f >= '0' && *f <= '9')
                    prec = prec * 10 + (*f++ - '0');
            }
        }
        while (*f == 'l' || *f == 'z')
            lng = 1, f++;
        char tmp[24];
        const char *s = tmp;
        int n = 0, neg = 0;
        switch (*f) {
        case 'd': case 'i': {
            long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
            neg = v < 0;
            do
                tmp[sizeof tmp - 1 - n++] = (char)('0' + u % 10);
            while (u /= 10);
            s = tmp + sizeof tmp - n;
            break;
        }
        case 'u': case 'x': case 'X': case 'p': {
            unsigned long u = *f == 'p' ? (unsigned long)va_arg(ap, void *)
                            : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            unsigned b = *f == 'u' ? 10 : 16;
            const char *dig = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do
                tmp[sizeof tmp - 1 - n++] = dig[u % b];
            while (u /= b);
            if (*f == 'p') {
                tmp[sizeof tmp - 1 - n++] = 'x';
                tmp[sizeof tmp - 1 - n++] = '0';
            }
            s = tmp + sizeof tmp - n;
            break;
        }
        case 's':
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (s[n] && (prec < 0 || n < prec))
                n++;
            zero = 0;
            break;
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            n = 1;
            zero = 0;
            break;
        case '%':
            tmp[0] = '%';
            n = 1;
            break;
        default:            // unknown conversion: print it literally
            put(&o, '%');
            if (!*f)
                return (int)o.len;
            put(&o, *f);
            continue;
        }
        int fill = width - n - neg;
        if (!left && !zero)
            pad(&o, ' ', fill);
        if (neg)
            put(&o, '-');
        if (!left && zero)
            pad(&o, '0', fill);
        for (int i = 0; i < n; i++)
            put(&o, s[i]);
        if (left)
            pad(&o, ' ', fill);
    }
    if (cap)
        buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int snprintf(char *buf, size_t cap, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(buf, cap, f, ap);
    va_end(ap);
    return n;
}

// shortcut: output over 1 KiB per call is truncated; stream it if a program needs more.
int vdprintf(int fd, const char *f, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, f, ap);
    write(fd, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    return n;
}

int dprintf(int fd, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int n = vdprintf(fd, f, ap);
    va_end(ap);
    return n;
}

int printf(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int n = vdprintf(1, f, ap);
    va_end(ap);
    return n;
}
