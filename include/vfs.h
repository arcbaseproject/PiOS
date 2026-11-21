#pragma once
#include <stddef.h>
#include <stdint.h>
#include "uapi.h"

struct inode;

struct dirent {             // filesystem-internal directory entry (readdir)
    uint32_t d_ino;
    uint32_t d_mode;        // file type bits of st_mode
    char d_name[NAME_MAX + 1];
};

// Per-inode operations. Directory ops are used only on directories.
// Buffers may be kernel pointers or already-validated user pointers.
struct inode_ops {
    long (*read)(struct inode *ino, char *buf, size_t len, uint64_t off);
    long (*write)(struct inode *ino, const char *buf, size_t len, uint64_t off);
    void (*truncate)(struct inode *ino);
    void (*close)(struct inode *ino, int flags);    // last close of an open file
    void (*release)(struct inode *ino);             // free fs data (refs 0, nlink 0)
    long (*lookup)(struct inode *dir, const char *name, struct inode **out);  // no ref taken
    long (*create)(struct inode *dir, const char *name, uint32_t mode, struct inode **out);
    long (*link)(struct inode *dir, const char *name, struct inode *ino);
    long (*unlink)(struct inode *dir, const char *name);
    long (*readdir)(struct inode *dir, uint64_t idx, struct dirent *out);   // 1 = entry, 0 = end
    long (*ioctl)(struct inode *ino, unsigned long req, uint64_t uarg);    // NULL = -ENOTTY
    int (*poll)(struct inode *ino);     // POLLIN/POLLOUT/POLLHUP now; NULL = always ready
};

struct inode {
    uint32_t mode;
    uint32_t ino;
    uint32_t nlink;         // directory entries naming it
    uint32_t refs;          // open files, cwds, in-kernel holders
    uint64_t size;
    const struct inode_ops *ops;
    void *priv;
    struct inode *parent;   // directories only
};

struct file {
    struct inode *ino;
    uint64_t off;
    int flags;
    int refs;               // fd slots sharing it (dup, fork)
};

#define NFD 32

struct inode *inode_new(uint32_t mode, const struct inode_ops *ops);
void iget(struct inode *ino);
void iput(struct inode *ino);
struct inode *vfs_root(void);
long vfs_lookup(const char *path, struct inode **out);
long vfs_lookup_nofollow(const char *path, struct inode **out);   // final symlink itself
long vfs_lookup_parent(const char *path, struct inode **dir, char *name);
long vfs_create(const char *path, uint32_t mode, struct inode **out);
struct file *file_new(struct inode *ino, int flags);
void file_put(struct file *f);
void vfs_init(void);

struct inode *ramfs_new_root(void);
void devfs_init(struct inode *dev);
void fat_mount_boot(void);      // SD card FAT32 -> /boot, if present
void console_input(char c);     // from UART RX IRQ
long sys_pipe2(uint64_t ufds, int flags);
