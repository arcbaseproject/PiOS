// File-related syscalls. Every user pointer is checked with vm_check first.
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "sched.h"
#include "timer.h"
#include "vfs.h"

static struct file *getf(int fd)
{
    if (fd < 0 || fd >= NFD)
        return NULL;
    return current_task()->fd[fd];
}

static int fd_install(struct file *f)
{
    struct task *t = current_task();
    for (int i = 0; i < NFD; i++)
        if (!t->fd[i]) {
            t->fd[i] = f;
            t->cloexec[i] = 0;
            return i;
        }
    return -EMFILE;
}

static long get_path(char *dst, uint64_t upath)
{
    long r = copyin_str(dst, upath, PATH_MAX);
    return r == -E2BIG ? -ENAMETOOLONG : r;
}

static int seekable(struct inode *ino) { return S_ISREG(ino->mode); }    // offsets advance on read/write

long sys_read(int fd, uint64_t ubuf, uint64_t len)
{
    struct file *f = getf(fd);
    if (!f || (f->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    if (S_ISDIR(f->ino->mode))
        return -EISDIR;
    if (!vm_check(current_task()->pgd, ubuf, len, 1))
        return -EFAULT;
    long n = f->ino->ops->read(f->ino, (char *)ubuf, len, f->off);
    if (n > 0 && seekable(f->ino))
        f->off += (uint64_t)n;
    return n;
}

long sys_write(int fd, uint64_t ubuf, uint64_t len)
{
    struct file *f = getf(fd);
    if (!f || (f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (!vm_check(current_task()->pgd, ubuf, len, 0))
        return -EFAULT;
    if (f->flags & O_APPEND)
        f->off = f->ino->size;
    long n = f->ino->ops->write(f->ino, (const char *)ubuf, len, f->off);
    if (n > 0 && seekable(f->ino))
        f->off += (uint64_t)n;
    return n;
}

long sys_open(uint64_t upath, int flags, int mode)
{
    char path[PATH_MAX];
    long r = get_path(path, upath);
    if (r < 0)
        return r;
    struct inode *ino;
    r = vfs_lookup(path, &ino);
    if (r == -ENOENT && (flags & O_CREAT))
        r = vfs_create(path, S_IFREG | (mode & 0777 & ~current_task()->umask), &ino);
    else if (r == 0 && (flags & O_CREAT) && (flags & O_EXCL)) {
        iput(ino);
        r = -EEXIST;
    }
    if (r < 0)
        return r;
    int acc = flags & O_ACCMODE;
    if ((flags & O_DIRECTORY) && !S_ISDIR(ino->mode)) {
        iput(ino);
        return -ENOTDIR;
    }
    if (S_ISDIR(ino->mode) && acc != O_RDONLY) {
        iput(ino);
        return -EISDIR;
    }
    if ((flags & O_TRUNC) && acc != O_RDONLY && S_ISREG(ino->mode))
        ino->ops->truncate(ino);
    struct file *f = file_new(ino, flags & (O_ACCMODE | O_APPEND | O_NONBLOCK));
    iput(ino);
    if (!f)
        return -ENOMEM;
    int fd = fd_install(f);
    if (fd < 0)
        file_put(f);
    else
        current_task()->cloexec[fd] = (flags & O_CLOEXEC) != 0;
    return fd;
}

long sys_close(int fd)
{
    struct file *f = getf(fd);
    if (!f)
        return -EBADF;
    current_task()->fd[fd] = NULL;
    file_put(f);
    return 0;
}

long sys_lseek(int fd, int64_t off, int whence)
{
    struct file *f = getf(fd);
    if (!f)
        return -EBADF;
    if (!seekable(f->ino) && !S_ISDIR(f->ino->mode))
        return -ESPIPE;     // directories: offset = entry index (rewinddir)
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->off :
                   whence == SEEK_END ? (int64_t)f->ino->size : -1;
    if (base < 0 || base + off < 0)
        return -EINVAL;
    f->off = (uint64_t)(base + off);
    return (long)f->off;
}

static long fill_stat(struct inode *ino, uint64_t ust)
{
    if (!vm_check(current_task()->pgd, ust, sizeof(struct stat), 1))
        return -EFAULT;
    struct stat *st = (struct stat *)ust;
    memset(st, 0, sizeof *st);
    st->st_dev = 1;
    st->st_ino = ino->ino;
    st->st_mode = ino->mode;
    st->st_nlink = ino->nlink ? ino->nlink : 1;
    st->st_size = S_ISREG(ino->mode) || S_ISLNK(ino->mode) ? (int64_t)ino->size : 0;
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    return 0;
}

long sys_stat(uint64_t upath, uint64_t ust)
{
    char path[PATH_MAX];
    struct inode *ino;
    long r = get_path(path, upath);
    if (r < 0 || (r = vfs_lookup(path, &ino)) < 0)
        return r;
    r = fill_stat(ino, ust);
    iput(ino);
    return r;
}

long sys_lstat(uint64_t upath, uint64_t ust)
{
    char path[PATH_MAX];
    struct inode *ino;
    long r = get_path(path, upath);
    if (r < 0 || (r = vfs_lookup_nofollow(path, &ino)) < 0)
        return r;
    r = fill_stat(ino, ust);
    iput(ino);
    return r;
}

long sys_fstat(int fd, uint64_t ust)
{
    struct file *f = getf(fd);
    return f ? fill_stat(f->ino, ust) : -EBADF;
}

long sys_dup(int fd)
{
    struct file *f = getf(fd);
    if (!f)
        return -EBADF;
    int nfd = fd_install(f);
    if (nfd >= 0)
        f->refs++;
    return nfd;
}

long sys_dup2(int fd, int nfd)
{
    struct file *f = getf(fd);
    if (!f || nfd < 0 || nfd >= NFD)
        return -EBADF;
    if (fd == nfd)
        return nfd;
    struct task *t = current_task();
    if (t->fd[nfd])
        file_put(t->fd[nfd]);
    f->refs++;
    t->fd[nfd] = f;
    t->cloexec[nfd] = 0;
    return nfd;
}

long sys_chdir(uint64_t upath)
{
    char path[PATH_MAX];
    struct inode *ino;
    long r = get_path(path, upath);
    if (r < 0 || (r = vfs_lookup(path, &ino)) < 0)
        return r;
    if (!S_ISDIR(ino->mode)) {
        iput(ino);
        return -ENOTDIR;
    }
    struct task *t = current_task();
    if (t->cwd)
        iput(t->cwd);
    t->cwd = ino;
    return 0;
}

// Name of `child` inside directory `dir`, by scanning entries.
static long name_in(struct inode *dir, struct inode *child, struct dirent *de)
{
    for (uint64_t i = 0; dir->ops->readdir(dir, i, de); i++)
        if (de->d_ino == child->ino)
            return 0;
    return -ENOENT;
}

// Returns length including NUL, as the Linux syscall does.
long sys_getcwd(uint64_t ubuf, uint64_t size)
{
    char buf[PATH_MAX];
    size_t pos = sizeof buf - 1;
    buf[pos] = 0;
    struct dirent de;
    struct inode *cur = current_task()->cwd ? current_task()->cwd : vfs_root();
    if (cur->nlink == 0)
        return -ENOENT;     // cwd was removed
    while (cur != vfs_root()) {
        if (name_in(cur->parent, cur, &de) < 0)
            return -ENOENT;
        size_t len = strlen(de.d_name);
        if (len + 1 > pos)
            return -ERANGE;
        pos -= len;
        memcpy(buf + pos, de.d_name, len);
        buf[--pos] = '/';
        cur = cur->parent;
    }
    if (pos == sizeof buf - 1)
        buf[--pos] = '/';
    size_t n = sizeof buf - pos;
    if (n > size)
        return -ERANGE;
    if (!vm_check(current_task()->pgd, ubuf, n, 1))
        return -EFAULT;
    memcpy((void *)ubuf, buf + pos, n);
    return (long)n;
}

long sys_mkdir(uint64_t upath, int mode)
{
    char path[PATH_MAX];
    struct inode *ino;
    long r = get_path(path, upath);
    if (r < 0 || (r = vfs_create(path, S_IFDIR | (mode & 0777), &ino)) < 0)
        return r;
    iput(ino);
    return 0;
}

// Shared by unlink and rmdir.
static long remove(uint64_t upath, int want_dir)
{
    char path[PATH_MAX], name[NAME_MAX + 1];
    struct inode *dir, *ino;
    long r = get_path(path, upath);
    if (r < 0 || (r = vfs_lookup_parent(path, &dir, name)) < 0)
        return r;
    struct dirent de;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        r = want_dir ? -EINVAL : -EISDIR;
    else if ((r = dir->ops->lookup(dir, name, &ino)) == 0) {
        if (want_dir && !S_ISDIR(ino->mode))
            r = -ENOTDIR;
        else if (!want_dir && S_ISDIR(ino->mode))
            r = -EISDIR;
        else if (want_dir && ino->ops->readdir(ino, 0, &de))
            r = -ENOTEMPTY;
        else {
            iget(ino);
            r = dir->ops->unlink(dir, name);
            iput(ino);      // frees it if nothing else holds it
        }
    }
    iput(dir);
    return r;
}

long sys_unlink(uint64_t upath) { return remove(upath, 0); }
long sys_rmdir(uint64_t upath) { return remove(upath, 1); }

long sys_rename(uint64_t uold, uint64_t unew)
{
    char opath[PATH_MAX], npath[PATH_MAX], oname[NAME_MAX + 1], nname[NAME_MAX + 1];
    struct inode *odir, *ndir, *ino, *victim;
    long r = get_path(opath, uold);
    if (r < 0 || (r = get_path(npath, unew)) < 0)
        return r;
    if ((r = vfs_lookup_parent(opath, &odir, oname)) < 0)
        return r;
    if ((r = vfs_lookup_parent(npath, &ndir, nname)) < 0) {
        iput(odir);
        return r;
    }
    struct dirent de;
    if (!strcmp(oname, ".") || !strcmp(oname, "..") || !strcmp(nname, ".") || !strcmp(nname, ".."))
        r = -EINVAL;
    else if (odir->ops != ndir->ops)
        r = -EXDEV;         // different filesystems
    else if ((r = odir->ops->lookup(odir, oname, &ino)) == 0) {
        iget(ino);
        // A directory may not move into itself or its own subtree.
        if (S_ISDIR(ino->mode))
            for (struct inode *a = ndir; ; a = a->parent) {
                if (a == ino) {
                    r = -EINVAL;
                    break;
                }
                if (a == vfs_root())
                    break;
            }
        if (r == 0 && ndir->ops->lookup(ndir, nname, &victim) == 0) {
            if (victim == ino)
                r = 1;              // same file: nothing to do
            else if (S_ISDIR(victim->mode) != S_ISDIR(ino->mode))
                r = S_ISDIR(victim->mode) ? -EISDIR : -ENOTDIR;
            else if (S_ISDIR(victim->mode) && victim->ops->readdir(victim, 0, &de))
                r = -ENOTEMPTY;
            else {
                iget(victim);
                ndir->ops->unlink(ndir, nname);
                iput(victim);
            }
        }
        if (r == 0 && (r = ndir->ops->link(ndir, nname, ino)) == 0)
            odir->ops->unlink(odir, oname);
        if (r == 1)
            r = 0;
        iput(ino);
    }
    iput(odir);
    iput(ndir);
    return r;
}

long sys_getdents64(int fd, uint64_t ubuf, uint64_t len)
{
    struct file *f = getf(fd);
    if (!f)
        return -EBADF;
    if (!S_ISDIR(f->ino->mode))
        return -ENOTDIR;
    if (!vm_check(current_task()->pgd, ubuf, len, 1))
        return -EFAULT;
    struct inode *dir = f->ino;
    uint64_t used = 0;
    for (;;) {
        struct dirent de = {0};
        if (f->off < 2) {           // synthesize "." and ".."
            struct inode *t = f->off ? dir->parent : dir;
            de.d_ino = t ? t->ino : dir->ino;
            de.d_mode = S_IFDIR;
            memcpy(de.d_name, "..", f->off + 1);
            de.d_name[f->off + 1] = 0;
        } else if (!dir->ops->readdir(dir, f->off - 2, &de)) {
            break;
        }
        size_t nl = strlen(de.d_name);
        uint64_t rec = (sizeof(struct dirent64) + nl + 1 + 7) & ~7ul;
        if (used + rec > len)
            return used ? (long)used : -EINVAL;
        struct dirent64 *d = (struct dirent64 *)(ubuf + used);
        d->d_ino = de.d_ino ? de.d_ino : 1;
        d->d_off = (int64_t)f->off + 1;
        d->d_reclen = (uint16_t)rec;
        uint32_t m = de.d_mode & S_IFMT;
        d->d_type = m == S_IFDIR ? DT_DIR : m == S_IFREG ? DT_REG : m == S_IFCHR ? DT_CHR :
                    m == S_IFIFO ? DT_FIFO : m == S_IFLNK ? DT_LNK : 0;
        memcpy(d->d_name, de.d_name, nl + 1);
        used += rec;
        f->off++;
    }
    return (long)used;
}

// ---- Linux *at() calls: dirfd is AT_FDCWD or a directory fd (used as the
// cwd for the duration of the call) ----

static long at_enter(int dirfd, struct inode **saved)
{
    struct task *t = current_task();
    *saved = t->cwd;
    if (dirfd == AT_FDCWD)
        return 0;
    struct file *f = getf(dirfd);
    if (!f)
        return -EBADF;
    if (!S_ISDIR(f->ino->mode))
        return -ENOTDIR;
    t->cwd = f->ino;            // borrowed: restored before the fd can go away
    return 0;
}

static void at_leave(struct inode *saved) { current_task()->cwd = saved; }

#define AT_CALL(dirfd, expr) ({ struct inode *sv_; long r_ = at_enter(dirfd, &sv_); \
                                if (r_ == 0) { r_ = (expr); at_leave(sv_); } r_; })

long sys_openat(int dirfd, uint64_t upath, int flags, int mode)
{
    return AT_CALL(dirfd, sys_open(upath, flags, mode));
}

long sys_newfstatat(int dirfd, uint64_t upath, uint64_t ust, int flags)
{
    char c = 0;
    if ((flags & 0x1000) && copyin_str(&c, upath, 1) >= 0 && !c)   // AT_EMPTY_PATH, ""
        return sys_fstat(dirfd, ust);
    if (flags & AT_SYMLINK_NOFOLLOW)
        return AT_CALL(dirfd, sys_lstat(upath, ust));
    return AT_CALL(dirfd, sys_stat(upath, ust));
}

long sys_mkdirat(int dirfd, uint64_t upath, int mode)
{
    return AT_CALL(dirfd, sys_mkdir(upath, mode & ~(int)current_task()->umask));
}

long sys_unlinkat(int dirfd, uint64_t upath, int flags)
{
    return AT_CALL(dirfd, flags & AT_REMOVEDIR ? sys_rmdir(upath) : sys_unlink(upath));
}

long sys_renameat(int olddir, uint64_t uold, int newdir, uint64_t unew)
{
    if (olddir != newdir && (olddir != AT_FDCWD || newdir != AT_FDCWD))
        return -EINVAL;         // shortcut: both names relative to the same directory
    return AT_CALL(olddir, sys_rename(uold, unew));
}

long sys_faccessat(int dirfd, uint64_t upath, int mode)
{
    (void)mode;                 // no permissions: existence is access
    char path[PATH_MAX];
    struct inode *ino;
    long r = AT_CALL(dirfd, get_path(path, upath) < 0 ? -EFAULT : vfs_lookup(path, &ino));
    if (r == 0)
        iput(ino);
    return r;
}

long sys_dup3(int fd, int nfd, int flags)
{
    if (fd == nfd)
        return -EINVAL;
    long r = sys_dup2(fd, nfd);
    if (r >= 0)
        current_task()->cloexec[nfd] = (flags & O_CLOEXEC) != 0;
    return r;
}

long sys_fcntl(int fd, int cmd, uint64_t arg)
{
    struct task *t = current_task();
    struct file *f = getf(fd);
    if (!f)
        return -EBADF;
    switch (cmd) {
    case F_DUPFD: case F_DUPFD_CLOEXEC:
        for (int i = (int)arg; i >= 0 && i < NFD; i++)
            if (!t->fd[i]) {
                f->refs++;
                t->fd[i] = f;
                t->cloexec[i] = cmd == F_DUPFD_CLOEXEC;
                return i;
            }
        return -EMFILE;
    case F_GETFD: return t->cloexec[fd] ? FD_CLOEXEC : 0;
    case F_SETFD: t->cloexec[fd] = (arg & FD_CLOEXEC) != 0; return 0;
    case F_GETFL: return f->flags;
    case F_SETFL: f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | ((int)arg & (O_APPEND | O_NONBLOCK)); return 0;
    }
    return -EINVAL;
}

long sys_ioctl(int fd, unsigned long req, uint64_t arg)
{
    struct file *f = getf(fd);
    if (!f)
        return -EBADF;
    return f->ino->ops->ioctl ? f->ino->ops->ioctl(f->ino, req, arg) : -ENOTTY;
}

// readv/writev: one read/write per buffer; stop at the first short transfer.
long sys_rwv(int fd, uint64_t uiov, int cnt, int write)
{
    if (cnt < 0 || cnt > 1024 || !vm_check(current_task()->pgd, uiov, (uint64_t)cnt * sizeof(struct iovec), 0))
        return cnt < 0 || cnt > 1024 ? -EINVAL : -EFAULT;
    const struct iovec *iov = (const struct iovec *)uiov;
    long total = 0;
    for (int i = 0; i < cnt; i++) {
        if (!iov[i].iov_len)
            continue;
        long n = write ? sys_write(fd, iov[i].iov_base, iov[i].iov_len)
                       : sys_read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0)
            return total ? total : n;
        total += n;
        if ((uint64_t)n < iov[i].iov_len)
            break;
    }
    return total;
}

// ppoll(fds, n, timeout, sigmask): readiness is rechecked every timer tick.
// shortcut: 10 ms polling instead of wait queues; sigmask is ignored.
long sys_ppoll(uint64_t ufds, uint64_t n, uint64_t uts)
{
    struct task *t = current_task();
    if (n > NFD * 4 || !vm_check(t->pgd, ufds, n * sizeof(struct pollfd), 1))
        return n > NFD * 4 ? -EINVAL : -EFAULT;
    int64_t limit = -1;                     // ticks; -1 = forever
    if (uts) {
        if (!vm_check(t->pgd, uts, sizeof(struct timespec), 0))
            return -EFAULT;
        const struct timespec *ts = (const struct timespec *)uts;
        limit = ts->tv_sec * HZ + (ts->tv_nsec + 1000000000 / HZ - 1) / (1000000000 / HZ);
    }
    uint64_t start = timer_ticks();
    struct pollfd *pf = (struct pollfd *)ufds;
    for (;;) {
        long ready = 0;
        for (uint64_t i = 0; i < n; i++) {
            pf[i].revents = 0;
            if (pf[i].fd < 0)
                continue;
            struct file *f = getf(pf[i].fd);
            int r = !f ? POLLNVAL : f->ino->ops->poll ? f->ino->ops->poll(f->ino) : POLLIN | POLLOUT;
            pf[i].revents = (int16_t)(r & (pf[i].events | POLLERR | POLLHUP | POLLNVAL));
            ready += pf[i].revents != 0;
        }
        if (ready || (limit >= 0 && (int64_t)(timer_ticks() - start) >= limit))
            return ready;
        if (tick_wait())
            return -EINTR;
    }
}

static long symlink(uint64_t utarget, uint64_t upath)
{
    char target[PATH_MAX], path[PATH_MAX];
    long r = get_path(target, utarget);
    if (r < 0 || (r = get_path(path, upath)) < 0)
        return r;
    if (!*target)
        return -ENOENT;
    struct inode *ino;
    if (vfs_lookup_nofollow(path, &ino) == 0) {
        iput(ino);
        return -EEXIST;
    }
    if ((r = vfs_create(path, S_IFLNK | 0777, &ino)) < 0)
        return r;
    size_t n = strlen(target);
    if (!S_ISLNK(ino->mode) || ino->ops->write(ino, target, n, 0) != (long)n) {
        iput(ino);              // filesystem without symlinks (FAT): undo
        sys_unlink(upath);
        return -EPERM;
    }
    iput(ino);
    return 0;
}

long sys_symlinkat(uint64_t utarget, int dirfd, uint64_t upath)
{
    return AT_CALL(dirfd, symlink(utarget, upath));
}

static long readlink(uint64_t upath, uint64_t ubuf, uint64_t size)
{
    char path[PATH_MAX];
    struct inode *ino;
    long r = get_path(path, upath);
    if (r < 0 || (r = vfs_lookup_nofollow(path, &ino)) < 0)
        return r;
    if (!S_ISLNK(ino->mode))
        r = -EINVAL;
    else if (!vm_check(current_task()->pgd, ubuf, size, 1))
        r = -EFAULT;
    else
        r = ino->ops->read(ino, (char *)ubuf, size < ino->size ? size : ino->size, 0);
    iput(ino);
    return r;
}

long sys_readlinkat(int dirfd, uint64_t upath, uint64_t ubuf, uint64_t size)
{
    return AT_CALL(dirfd, readlink(upath, ubuf, size));
}
