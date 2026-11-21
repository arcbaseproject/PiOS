#pragma once
#include <stddef.h>
#include <stdint.h>
#include "exception.h"

_Noreturn void do_exit(int status);
void proc_create_init(void);
long sys_fork(struct trap_frame *tf);
long sys_execve(struct trap_frame *tf, uint64_t upath, uint64_t uargv, uint64_t uenvp);
long sys_waitpid(int pid, uint64_t ustatus, int options);
long syscall_dispatch(struct trap_frame *tf);
struct inode;
int elf_load(uint64_t *pgd, struct inode *ino, uint64_t *entry);
long copyin_str(char *dst, uint64_t uva, size_t max);

struct initrd_file { const char *name; const uint8_t *start, *end; };
extern const struct initrd_file initrd_table[];   // copied into ramfs at boot

long sys_read(int fd, uint64_t ubuf, uint64_t len);
long sys_write(int fd, uint64_t ubuf, uint64_t len);
long sys_open(uint64_t upath, int flags, int mode);
long sys_close(int fd);
long sys_lseek(int fd, int64_t off, int whence);
long sys_stat(uint64_t upath, uint64_t ust);
long sys_fstat(int fd, uint64_t ust);
long sys_dup(int fd);
long sys_dup2(int fd, int nfd);
long sys_chdir(uint64_t upath);
long sys_getcwd(uint64_t ubuf, uint64_t size);
long sys_mkdir(uint64_t upath, int mode);
long sys_unlink(uint64_t upath);
long sys_rmdir(uint64_t upath);
long sys_rename(uint64_t uold, uint64_t unew);
long sys_getdents(int fd, uint64_t ubuf, uint64_t len);
