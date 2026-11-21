// libm subset. shortcut: Taylor/series kernels after range reduction; errors of
// a few ulp, trig reduction exact only for |x| < ~1e9.
#include "math.h"
#include <stdint.h>

static const double LN2_HI = 6.93147180369123816490e-01, LN2_LO = 1.90821492927058770002e-10;
static const double PIO2_1 = 1.57079632673412561417e+00, PIO2_2 = 6.07710050650619224932e-11,
                    PIO2_3 = 2.02226624879595063154e-21;
static const double PI = 3.14159265358979311600e+00, PIO2 = 1.57079632679489655800e+00;

static uint64_t bits(double x) { union { double d; uint64_t u; } v = { x }; return v.u; }
static double from_bits(uint64_t u) { union { uint64_t u; double d; } v = { u }; return v.d; }

double fabs(double x) { return from_bits(bits(x) & ~(1ull << 63)); }
double sqrt(double x) { __asm__("fsqrt %d0, %d1" : "=w"(x) : "w"(x)); return x; }
double floor(double x) { __asm__("frintm %d0, %d1" : "=w"(x) : "w"(x)); return x; }
double ceil(double x) { __asm__("frintp %d0, %d1" : "=w"(x) : "w"(x)); return x; }
double trunc(double x) { __asm__("frintz %d0, %d1" : "=w"(x) : "w"(x)); return x; }

double frexp(double x, int *e)
{
    uint64_t u = bits(x);
    int ex = (int)(u >> 52 & 0x7ff);
    if (ex == 0) {                      // zero or subnormal
        if (x == 0) {
            *e = 0;
            return x;
        }
        x *= 0x1p64;
        double r = frexp(x, e);
        *e -= 64;
        return r;
    }
    if (ex == 0x7ff) {                  // inf / nan
        *e = 0;
        return x;
    }
    *e = ex - 1022;
    return from_bits((u & ~(0x7ffull << 52)) | (1022ull << 52));
}

double ldexp(double x, int e)
{
    while (e > 1023) {
        x *= 0x1p1023;
        e -= 1023;
        if (x == HUGE_VAL || x == -HUGE_VAL)
            return x;
    }
    while (e < -1022) {
        x *= 0x1p-1022;
        e += 1022;
        if (x == 0)
            return x;
    }
    return x * from_bits((uint64_t)(e + 1023) << 52);
}

// Exact remainder: subtract the largest y * 2^k <= |x| until |x| < |y|.
double fmod(double x, double y)
{
    if (y == 0 || x != x || y != y || fabs(x) == HUGE_VAL)
        return NAN;
    double ax = fabs(x), ay = fabs(y);
    while (ax >= ay) {
        int ex, ey;
        frexp(ax, &ex);
        frexp(ay, &ey);
        double t = ldexp(ay, ex - ey);
        if (t > ax)
            t /= 2;
        ax -= t;
    }
    return x < 0 ? -ax : ax;
}

double exp(double x)
{
    if (x != x)
        return x;
    if (x > 709.8)
        return HUGE_VAL;
    if (x < -745.2)
        return 0;
    double k = floor(x / (LN2_HI + LN2_LO) + 0.5);
    double r = (x - k * LN2_HI) - k * LN2_LO;   // |r| <= ln2/2
    double term = 1, sum = 1;
    for (int i = 1; i < 16; i++) {
        term *= r / i;
        sum += term;
    }
    return ldexp(sum, (int)k);
}

double log(double x)
{
    if (x != x || x < 0)
        return NAN;
    if (x == 0)
        return -HUGE_VAL;
    if (x == HUGE_VAL)
        return x;
    int k;
    double m = frexp(x, &k);            // x = m * 2^k, m in [0.5, 1)
    if (m < 0.70710678118654752440) {
        m *= 2;
        k--;
    }
    double s = (m - 1) / (m + 1), s2 = s * s, term = s, sum = 0;
    for (int i = 1; i < 40; i += 2) {   // log(m) = 2 atanh(s), |s| < 0.172
        sum += term / i;
        term *= s2;
    }
    return 2 * sum + k * LN2_LO + k * LN2_HI;
}

double log2(double x)
{
    if (x != x || x <= 0 || x == HUGE_VAL)
        return log(x);
    int k;
    double m = frexp(x, &k);
    if (m == 0.5)
        return k - 1;                   // exact powers of two
    return log(m) / (LN2_HI + LN2_LO) + k;
}

double log10(double x)
{
    double r = log(x) / 2.30258509299404568402;
    double n = floor(r + 0.5);          // snap exact powers of ten
    if (fabs(r - n) < 1e-12 && n >= 0 && n <= 22) {
        double p = 1;
        for (int i = 0; i < n; i++)
            p *= 10;
        if (p == x)
            return n;
    }
    return r;
}

double pow(double x, double y)
{
    if (y == 0 || x == 1)
        return 1;
    if (x != x || y != y)
        return NAN;
    if (y == trunc(y) && fabs(y) < 0x1p31) {   // integer exponent: square and multiply
        long n = (long)y;
        unsigned long u = n < 0 ? -(unsigned long)n : (unsigned long)n;
        double r = 1, b = x;
        for (; u; u >>= 1, b *= b)
            if (u & 1)
                r *= b;
        return n < 0 ? 1 / r : r;
    }
    if (x < 0) {
        if (y != trunc(y))
            return NAN;
        double r = exp(y * log(-x));
        return fmod(y, 2) != 0 ? -r : r;
    }
    if (x == 0)
        return y > 0 ? 0 : HUGE_VAL;
    return exp(y * log(x));
}

// sin/cos kernels on |r| <= pi/4.
static double ksin(double r)
{
    double r2 = r * r, term = r, sum = r;
    for (int i = 3; i < 22; i += 2) {
        term *= -r2 / ((i - 1) * i);
        sum += term;
    }
    return sum;
}

static double kcos(double r)
{
    double r2 = r * r, term = 1, sum = 1;
    for (int i = 2; i < 22; i += 2) {
        term *= -r2 / ((i - 1) * i);
        sum += term;
    }
    return sum;
}

// x = q * pi/2 + r; returns q mod 4.
static int reduce(double x, double *r)
{
    double q = floor(x / PIO2 + 0.5);
    *r = ((x - q * PIO2_1) - q * PIO2_2) - q * PIO2_3;
    return (int)((long)fmod(q, 4) + 4) & 3;
}

double sin(double x)
{
    if (x != x || fabs(x) == HUGE_VAL)
        return NAN;
    double r;
    switch (reduce(x, &r)) {
    case 0: return ksin(r);
    case 1: return kcos(r);
    case 2: return -ksin(r);
    default: return -kcos(r);
    }
}

double cos(double x)
{
    if (x != x || fabs(x) == HUGE_VAL)
        return NAN;
    double r;
    switch (reduce(x, &r)) {
    case 0: return kcos(r);
    case 1: return -ksin(r);
    case 2: return -kcos(r);
    default: return ksin(r);
    }
}

double tan(double x) { return sin(x) / cos(x); }

double atan(double x)
{
    if (x != x)
        return x;
    int neg = x < 0, inv = 0, shift = 0;
    if (neg)
        x = -x;
    if (x > 1) {
        x = 1 / x;
        inv = 1;
    }
    if (x > 0.26794919243112270) {      // tan(pi/12): atan(x) = pi/6 + atan((x*sqrt3-1)/(sqrt3+x))
        const double s3 = 1.73205080756887729353;
        x = (x * s3 - 1) / (s3 + x);
        shift = 1;
    }
    double x2 = x * x, term = x, sum = 0;
    for (int i = 1; i < 60; i += 2) {
        sum += term / i;
        term *= -x2;
    }
    if (shift)
        sum += PI / 6;
    if (inv)
        sum = PIO2 - sum;
    return neg ? -sum : sum;
}

double atan2(double y, double x)
{
    if (x != x || y != y)
        return NAN;
    if (x > 0)
        return atan(y / x);
    if (x < 0)
        return y >= 0 ? atan(y / x) + PI : atan(y / x) - PI;
    return y > 0 ? PIO2 : y < 0 ? -PIO2 : 0;
}

double asin(double x) { return fabs(x) > 1 ? NAN : atan2(x, sqrt(1 - x * x)); }
double acos(double x) { return fabs(x) > 1 ? NAN : atan2(sqrt(1 - x * x), x); }
