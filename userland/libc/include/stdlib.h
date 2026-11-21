#pragma once
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff
void *malloc(size_t n);
void free(void *p);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
_Noreturn void exit(int code);
_Noreturn void abort(void);
char *getenv(const char *name);
int system(const char *cmd);
double strtod(const char *s, char **end);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long atol(const char *s);
int atoi(const char *s);
int abs(int v);
long labs(long v);
int rand(void);
void srand(unsigned seed);
