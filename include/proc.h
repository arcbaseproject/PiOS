#pragma once
#include <stddef.h>
#include <stdint.h>
#include "exception.h"

_Noreturn void do_exit(int status);
void proc_create_init(void);
long sys_fork(struct trap_frame *tf);
long sys_execve(struct trap_frame *tf, uint64_t upath, uint64_t uargv, uint64_t uenvp);
long sys_wait4(int pid, uint64_t ustatus, int options);
long syscall_dispatch(struct trap_frame *tf);
struct inode;
struct elf_info { uint64_t entry, top, phdr, phnum; };     // top = page-rounded image end
int elf_load(uint64_t *pgd, struct inode *ino, struct elf_info *info);
long sys_brk(uint64_t addr);
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
long sys_getdents64(int fd, uint64_t ubuf, uint64_t len);
long sys_kill(int pid, int sig);
long sys_setpgid(int pid, int pgid);
long sys_getprocs(uint64_t ubuf, int max);
void kill_pgrp(int pgid, int sig);
long sys_rt_sigaction(int sig, uint64_t uact, uint64_t uold, uint64_t size);
long sys_rt_sigprocmask(int how, uint64_t uset, uint64_t uold, uint64_t size);
long sys_rt_sigreturn(struct trap_frame *tf);
long sys_openat(int dirfd, uint64_t upath, int flags, int mode);
long sys_newfstatat(int dirfd, uint64_t upath, uint64_t ust, int flags);
long sys_mkdirat(int dirfd, uint64_t upath, int mode);
long sys_unlinkat(int dirfd, uint64_t upath, int flags);
long sys_renameat(int olddir, uint64_t uold, int newdir, uint64_t unew);
long sys_faccessat(int dirfd, uint64_t upath, int mode);
long sys_dup3(int fd, int nfd, int flags);
long sys_fcntl(int fd, int cmd, uint64_t arg);
long sys_ioctl(int fd, unsigned long req, uint64_t arg);
long sys_rwv(int fd, uint64_t uiov, int cnt, int write);
long sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, int64_t off);
long sys_munmap(uint64_t addr, uint64_t len);
long sys_clone(struct trap_frame *tf, uint64_t flags, uint64_t stack);
long sys_uname(uint64_t ubuf);
long sys_getpgid(int pid);
long sys_setsid(void);
long sys_getppid(void);
long sys_umask(int mask);
long sys_ppoll(uint64_t ufds, uint64_t n, uint64_t uts);
long sys_lstat(uint64_t upath, uint64_t ust);
long sys_symlinkat(uint64_t utarget, int dirfd, uint64_t upath);
long sys_readlinkat(int dirfd, uint64_t upath, uint64_t ubuf, uint64_t size);
void signal_deliver(struct trap_frame *tf);     // on return to EL0: exit if killed, else enter a handler
