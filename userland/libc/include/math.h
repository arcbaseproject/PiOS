// shortcut: series/polynomial implementations, accurate to a few ulp, not
// correctly rounded; pow is exp(y*log(x)) except for integer exponents.
#pragma once
#define HUGE_VAL __builtin_huge_val()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
double fabs(double x);
double sqrt(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double fmod(double x, double y);
double frexp(double x, int *e);
double ldexp(double x, int e);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
