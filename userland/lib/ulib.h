#pragma once
#include <stddef.h>
#include "uapi.h"

/* Raw syscalls: return -errno on failure (no errno variable yet). */
_Noreturn void exit(int code);    /* runs __exit_hook (stdio flush), then _exit */
_Noreturn void _exit(int code);
extern void (*__exit_hook)(void);
extern char **environ;
int fork(void);
long write(int fd, const void *buf, size_t len);
int getpid(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int waitpid(int pid, int *status, int options);
long read(int fd, void *buf, size_t len);
int open(const char *path, int flags, ...);
int close(int fd);
long lseek(int fd, long off, int whence);
int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int dup(int fd);
int dup2(int fd, int nfd);
int pipe(int fds[2]);
int chdir(const char *path);
long getcwd(char *buf, size_t size);   /* returns length incl. NUL */
int mkdir(const char *path, int mode);
int unlink(const char *path);
int rmdir(const char *path);
int rename(const char *from, const char *to);
long getdents(int fd, struct dirent *buf, size_t len);
int kill(int pid, int sig);
int setpgid(int pid, int pgid);
int tcsetpgrp(int pgid);
int ttymode(int mode);
long sigaction(int sig, void *handler, void *restorer);   /* raw; use signal() */
typedef void (*sighandler_t)(int);
sighandler_t signal(int sig, sighandler_t h);   /* h may be (sighandler_t)SIG_DFL / SIG_IGN; returns old or (sighandler_t)-errno */
int getprocs(struct procinfo *buf, int max);
long brk(void *addr);           /* raw: returns the new (or unchanged) break */
int nanosleep(const struct timespec *req, struct timespec *rem);   /* rem unused */
int clock_gettime(int clk, struct timespec *ts);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
void *memchr(const void *s, int c, size_t n);
long atol(const char *s);

void *sbrk(long inc);           /* (void *)-1 on failure */
void *malloc(size_t n);
void free(void *p);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);

#include <stdarg.h>
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vdprintf(int fd, const char *fmt, va_list ap);
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void puts(const char *s);       /* no newline added */
void putnum(long v);
void eputs(const char *s);      /* to fd 2 */
void err(const char *who, const char *what, long e);   /* "who: what: message\n" to fd 2 */
