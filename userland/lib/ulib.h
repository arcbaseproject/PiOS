#pragma once
#include <stddef.h>
#include "uapi.h"

/* Raw syscalls: return -errno on failure (no errno variable yet). */
_Noreturn void exit(int code);
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

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void puts(const char *s);       /* no newline added */
void putnum(long v);
