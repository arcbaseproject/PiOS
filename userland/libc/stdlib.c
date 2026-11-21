// stdlib.h pieces beyond ulib: strtod/strtol, getenv, abort, system, rand.
#include "ulib.h"
#include "ctype.h"
#include "errno.h"
#include "stdio.h"
#include "stdlib.h"

_Noreturn void abort(void)
{
    fflush(NULL);
    kill(getpid(), SIGKILL);
    _exit(134);
}

char *getenv(const char *name)
{
    size_t n = strlen(name);
    for (char **e = environ; e && *e; e++)
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=')
            return *e + n + 1;
    return NULL;
}

int system(const char *cmd)
{
    return cmd ? -1 : 0;        // no "sh -c" yet; 0 = no shell available
}

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
int atoi(const char *s) { return (int)atol(s); }

static unsigned long seed = 1;
int rand(void)
{
    seed = seed * 6364136223846793005ul + 1442695040888963407ul;
    return (int)(seed >> 33);
}
void srand(unsigned s) { seed = s; }

unsigned long strtoul(const char *s, char **end, int base)
{
    const char *p = s;
    while (isspace(*p))
        p++;
    int neg = *p == '-';
    if (*p == '-' || *p == '+')
        p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 0x20) == 'x' && isxdigit(p[2]))
        p += 2, base = 16;
    else if (base == 0)
        base = *p == '0' ? 8 : 10;
    unsigned long v = 0;
    const char *start = p;
    for (;; p++) {
        int d = isdigit(*p) ? *p - '0' : isalpha(*p) ? (*p | 0x20) - 'a' + 10 : 99;
        if (d >= base)
            break;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (end)
        *end = (char *)(p == start ? s : p);
    return neg ? -v : v;
}

long strtol(const char *s, char **end, int base)
{
    return (long)strtoul(s, end, base);
}

// Decimal only (Lua parses hex floats itself). shortcut: up to 19 significant
// digits scaled by powers of ten in double; last-ulp errors possible.
double strtod(const char *s, char **end)
{
    const char *p = s;
    while (isspace(*p))
        p++;
    int neg = *p == '-';
    if (*p == '-' || *p == '+')
        p++;
    if (strncmp(p, "inf", 3) == 0 || strncmp(p, "INF", 3) == 0) {
        if (end)
            *end = (char *)p + 3;
        return neg ? -__builtin_inf() : __builtin_inf();
    }
    unsigned long m = 0;
    int digits = 0, exp10 = 0, any = 0;
    for (; isdigit(*p); p++, any = 1)
        if (digits < 19)
            m = m * 10 + (unsigned long)(*p - '0'), digits += m != 0;
        else
            exp10++;
    if (*p == '.')
        for (p++; isdigit(*p); p++, any = 1)
            if (digits < 19)
                m = m * 10 + (unsigned long)(*p - '0'), digits += m != 0, exp10--;
    if (!any) {
        if (end)
            *end = (char *)s;
        return 0;
    }
    if ((*p | 0x20) == 'e') {
        const char *q = p + 1;
        int eneg = *q == '-';
        if (*q == '-' || *q == '+')
            q++;
        if (isdigit(*q)) {
            int e = 0;
            for (; isdigit(*q); q++)
                if (e < 10000)
                    e = e * 10 + (*q - '0');
            exp10 += eneg ? -e : e;
            p = q;
        }
    }
    if (end)
        *end = (char *)p;
    double v = (double)m;
    if (m) {
        // Divide by exact powers where possible: more accurate than multiplying by 1e-n.
        while (exp10 > 22)
            v *= 1e22, exp10 -= 22;
        while (exp10 < -22 && v != 0)
            v /= 1e22, exp10 += 22;
        static const double p10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10,
                                      1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
                                      1e20, 1e21, 1e22 };
        v = exp10 >= 0 ? v * p10[exp10] : v / p10[-exp10];
        if (v > 1.7976931348623157e308 || v == 0)
            errno = ERANGE;
    }
    return neg ? -v : v;
}
