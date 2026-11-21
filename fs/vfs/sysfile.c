// File-related syscalls. Every user pointer is checked with vm_check first.
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "sched.h"
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
            return i;
        }
    return -EMFILE;
}

static long get_path(char *dst, uint64_t upath)
{
    long r = copyin_str(dst, upath, PATH_MAX);
    return r == -E2BIG ? -ENAMETOOLONG : r;
}

static int seekable(struct inode *ino) { return S_ISREG(ino->mode); }

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
        r = vfs_create(path, S_IFREG | (mode & 0777), &ino);
    else if (r == 0 && (flags & O_CREAT) && (flags & O_EXCL)) {
        iput(ino);
        r = -EEXIST;
    }
    if (r < 0)
        return r;
    int acc = flags & O_ACCMODE;
    if (S_ISDIR(ino->mode) && acc != O_RDONLY) {
        iput(ino);
        return -EISDIR;
    }
    if ((flags & O_TRUNC) && acc != O_RDONLY && S_ISREG(ino->mode))
        ino->ops->truncate(ino);
    struct file *f = file_new(ino, flags);
    iput(ino);
    if (!f)
        return -ENOMEM;
    int fd = fd_install(f);
    if (fd < 0)
        file_put(f);
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
    if (!seekable(f->ino))
        return -ESPIPE;
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
    st->st_ino = ino->ino;
    st->st_mode = ino->mode;
    st->st_nlink = ino->nlink;
    st->st_pad = 0;
    st->st_size = S_ISREG(ino->mode) ? ino->size : 0;
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

long sys_getdents(int fd, uint64_t ubuf, uint64_t len)
{
    struct file *f = getf(fd);
    if (!f)
        return -EBADF;
    if (!S_ISDIR(f->ino->mode))
        return -ENOTDIR;
    if (!vm_check(current_task()->pgd, ubuf, len, 1))
        return -EFAULT;
    struct inode *dir = f->ino;
    struct dirent *out = (struct dirent *)ubuf;
    long n = 0;
    while ((uint64_t)(n + 1) * sizeof *out <= len) {
        struct dirent de = {0};
        if (f->off < 2) {           // synthesize "." and ".."
            struct inode *t = f->off ? dir->parent : dir;
            de.d_ino = t->ino;
            de.d_mode = S_IFDIR;
            memcpy(de.d_name, "..", f->off + 1);
            de.d_name[f->off + 1] = 0;
        } else if (!dir->ops->readdir(dir, f->off - 2, &de)) {
            break;
        }
        out[n++] = de;
        f->off++;
    }
    return n * (long)sizeof *out;
}
