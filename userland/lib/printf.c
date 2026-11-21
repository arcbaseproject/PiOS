// printf family: %d %i %u %o %x %X %p %s %c %f %F %e %E %g %G %%, flags - + space
// # 0, width, .precision, '*' for both, length h hh l ll z j t.
// shortcut: floats are scaled by powers of ten in double arithmetic, so digits
// past ~15 significant ones can be off by one ulp (not correctly rounded);
// fine for display and Lua's "%.14g", not for exact round-tripping.
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

static double pow10i(int n)     // 10^n, n >= 0; exact up to 1e22
{
    static const double t[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    double r = 1;
    for (; n > 22; n -= 22)
        r *= 1e22;
    return r * t[n];
}

// v * 10^n without overflowing intermediate powers.
static double scale10(double v, int n)
{
    while (n > 300)
        v *= 1e300, n -= 300;
    while (n < -300)
        v /= 1e300, n += 300;
    return n >= 0 ? v * pow10i(n) : v / pow10i(-n);
}

// Round v > 0 to p significant digits (1..17): digits into d, returns the
// decimal exponent of the first digit.
static int decimal(double v, int p, char *d)
{
    int b2 = 0;
    for (double t = v; t >= 2; t /= 2)
        b2++;
    for (double t = v; t < 1; t *= 2)
        b2--;
    int e = (b2 * 30103) / 100000 - (b2 < 0);     // ~floor(log10(v)), may be one low
    uint64_t lim = (uint64_t)pow10i(p), n;
    for (;;) {
        double s = scale10(v, p - 1 - e);
        n = (uint64_t)(s + 0.5);
        if (n >= lim)
            e++;
        else if (n < lim / 10)
            e--;
        else
            break;
    }
    for (int i = p - 1; i >= 0; i--, n /= 10)
        d[i] = (char)('0' + n % 10);
    return e;
}

// Format a finite v >= 0 for %e/%f/%g into s; returns length.
static int fmt_float(char *s, double v, char conv, int prec, int alt)
{
    char d[18];
    int n = 0, upper = conv == 'E' || conv == 'G';
    conv |= 0x20;
    if (prec < 0)
        prec = 6;
    if (conv == 'g') {
        int p = prec ? prec : 1, x = 0;
        if (v != 0)
            x = decimal(v, p > 17 ? 17 : p, d);
        if (x < -4 || x >= p) {
            conv = 'e', prec = p - 1;
        } else {
            conv = 'f', prec = p - 1 - x;
        }
        n = fmt_float(s, v, conv, prec, alt);
        if (!alt && memchr(s, '.', (size_t)n)) {     // %g drops trailing zeros
            char *e = memchr(s, 'e', (size_t)n);
            int end = e ? (int)(e - s) : n, k = end;
            while (s[k - 1] == '0')
                k--;
            if (s[k - 1] == '.')
                k--;
            memmove(s + k, s + end, (size_t)(n - end));
            n -= end - k;
        }
        if (upper)
            for (int i = 0; i < n; i++)
                if (s[i] == 'e')
                    s[i] = 'E';
        return n;
    }
    if (conv == 'e') {
        int sig = prec + 1, x = 0;
        if (v != 0)
            x = decimal(v, sig > 17 ? 17 : sig, d);
        else
            memset(d, '0', 17);
        s[n++] = d[0];
        if (prec || alt)
            s[n++] = '.';
        for (int i = 1; i < sig; i++)
            s[n++] = i < 17 ? d[i] : '0';
        s[n++] = upper ? 'E' : 'e';
        s[n++] = x < 0 ? '-' : '+';
        if (x < 0)
            x = -x;
        if (x >= 100)
            s[n++] = (char)('0' + x / 100);
        s[n++] = (char)('0' + x / 10 % 10);
        s[n++] = (char)('0' + x % 10);
        return n;
    }
    // %f: digits up to 10^x, then prec decimals.
    int x = 0, sig = 0;
    if (v != 0) {
        x = decimal(v, 17, d);
        sig = x + 1 + prec;             // significant digits actually shown
        if (sig > 17)
            sig = 17;
        if (sig >= 1)
            x = decimal(v, sig, d);     // round at the last shown place
        else if (sig == 0 && scale10(v, prec) >= 0.5)
            d[0] = '1', x++, sig = 1;   // rounds up into the last place
    }
    for (int i = x < 0 ? 0 : x; i >= 0; i--) {        // integer part
        int k = x - i;
        s[n++] = x < 0 ? '0' : (k < sig ? d[k] : '0');
    }
    if (prec || alt)
        s[n++] = '.';
    for (int i = 1; i <= prec; i++) {
        int k = x + i;                  // index of digit at 10^-i
        s[n++] = k >= 0 && k < sig ? d[k] : '0';
    }
    return n;
}

int vsnprintf(char *buf, size_t cap, const char *f, va_list ap)
{
    struct out o = { buf, cap, 0 };
    for (; *f; f++) {
        if (*f != '%') {
            put(&o, *f);
            continue;
        }
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, prec = -1, lng = 0;
        for (;; f++) {
            if (f[1] == '-')
                left = 1;
            else if (f[1] == '0')
                zero = 1;
            else if (f[1] == '+')
                plus = 1;
            else if (f[1] == ' ')
                space = 1;
            else if (f[1] == '#')
                alt = 1;
            else
                break;
        }
        f++;
        if (*f == '*') {
            width = va_arg(ap, int);
            if (width < 0)
                left = 1, width = -width;
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
        while (*f == 'l' || *f == 'z' || *f == 'j' || *f == 't' || *f == 'h')
            lng |= *f++ != 'h';
        char tmp[400];
        const char *s = tmp;
        const char *prefix = "";
        int n = 0;
        switch (*f) {
        case 'd': case 'i': {
            long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
            prefix = v < 0 ? "-" : plus ? "+" : space ? " " : "";
            do
                tmp[sizeof tmp - 1 - n++] = (char)('0' + u % 10);
            while (u /= 10);
            s = tmp + sizeof tmp - n;
            break;
        }
        case 'u': case 'o': case 'x': case 'X': case 'p': {
            unsigned long u = *f == 'p' ? (unsigned long)va_arg(ap, void *)
                            : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            unsigned b = *f == 'u' ? 10 : *f == 'o' ? 8 : 16;
            const char *dig = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do
                tmp[sizeof tmp - 1 - n++] = dig[u % b];
            while (u /= b);
            if (*f == 'p' || (alt && *f == 'x' && u))
                prefix = "0x";
            else if (alt && *f == 'X')
                prefix = "0X";
            else if (alt && *f == 'o')
                prefix = "0";
            s = tmp + sizeof tmp - n;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double v = va_arg(ap, double);
            int neg = v < 0 || (v == 0 && 1 / v < 0);
            if (neg)
                v = -v;
            prefix = neg ? "-" : plus ? "+" : space ? " " : "";
            int up = *f == 'F' || *f == 'E' || *f == 'G';
            if (v != v) {
                s = up ? "NAN" : "nan", n = 3, zero = 0;
            } else if (v > 1.7976931348623157e308) {
                s = up ? "INF" : "inf", n = 3, zero = 0;
            } else {
                if (prec > 340)
                    prec = 340;
                if ((*f | 0x20) == 'f' && v >= 1e40)   // keep within tmp; shortcut: no %f past 1e40
                    n = fmt_float(tmp, v, up ? 'E' : 'e', prec, alt);
                else
                    n = fmt_float(tmp, v, *f, prec, alt);
            }
            prec = -1;                  // consumed; not an integer precision below
            break;
        }
        case 's':
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (s[n] && (prec < 0 || n < prec))
                n++;
            zero = 0;
            prec = -1;
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
        int pl = (int)strlen(prefix);
        int zeros = prec > n && *f != 'c' && *f != '%' ? prec - n : 0;   // integer precision
        int fill = width - n - pl - zeros;
        if (!left && (!zero || prec >= 0))
            pad(&o, ' ', fill);
        for (int i = 0; i < pl; i++)
            put(&o, prefix[i]);
        if (!left && zero && prec < 0)
            pad(&o, '0', fill);
        pad(&o, '0', zeros);
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
