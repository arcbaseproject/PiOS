// Minimal stdio for ported programs (Lua). Buffered FILE over PiOS fds.
#pragma once
#include <stddef.h>
#include <stdarg.h>

typedef struct FILE FILE;
extern FILE *stdin, *stdout, *stderr;

#define EOF (-1)
#define BUFSIZ 1024
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#define L_tmpnam 32

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);
int fclose(FILE *f);
int fflush(FILE *f);
size_t fread(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int ungetc(int c, FILE *f);
char *fgets(char *s, int n, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vfprintf(FILE *f, const char *fmt, va_list ap);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
FILE *tmpfile(void);
char *tmpnam(char *s);
int remove(const char *path);
int rename(const char *from, const char *to);
void perror(const char *s);
