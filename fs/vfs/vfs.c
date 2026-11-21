// VFS core: inode lifetime, path resolution, open file objects, boot tree.
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "sched.h"
#include "vfs.h"

static struct inode *root;
static uint32_t next_ino = 1;

struct inode *vfs_root(void) { return root; }

struct inode *inode_new(uint32_t mode, const struct inode_ops *ops)
{
    struct inode *ino = kmalloc(sizeof *ino);
    if (ino) {
        ino->mode = mode;
        ino->ops = ops;
        ino->ino = next_ino++;
    }
    return ino;
}

void iget(struct inode *ino) { ino->refs++; }

void iput(struct inode *ino)
{
    if (--ino->refs == 0 && ino->nlink == 0) {
        if (ino->ops->release)
            ino->ops->release(ino);
        kfree(ino);
    }
}

// Resolve path. If `last` is non-NULL, stop before the final component, copy
// it into `last` and return its directory. Result carries a reference.
static long walk(const char *path, struct inode **out, char *last)
{
    struct task *t = current_task();
    struct inode *cur = (*path == '/' || !t->cwd) ? root : t->cwd;
    const char *p = path;
    char comp[NAME_MAX + 1];
    if (!*p)
        return -ENOENT;
    for (;;) {
        while (*p == '/')
            p++;
        if (!*p) {
            if (last)
                memcpy(last, ".", 2);   // "/" or "dir/": no final name
            break;
        }
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - s);
        if (len > NAME_MAX)
            return -ENAMETOOLONG;
        memcpy(comp, s, len);
        comp[len] = 0;
        const char *q = p;
        while (*q == '/')
            q++;
        if (last && !*q) {
            memcpy(last, comp, len + 1);
            break;
        }
        if (!S_ISDIR(cur->mode))
            return -ENOTDIR;
        if (strcmp(comp, ".") == 0)
            continue;
        if (strcmp(comp, "..") == 0) {
            cur = cur->parent;
            continue;
        }
        long r = cur->ops->lookup(cur, comp, &cur);
        if (r < 0)
            return r;
    }
    if (last && !S_ISDIR(cur->mode))
        return -ENOTDIR;
    iget(cur);
    *out = cur;
    return 0;
}

long vfs_lookup(const char *path, struct inode **out) { return walk(path, out, NULL); }
long vfs_lookup_parent(const char *path, struct inode **dir, char *name) { return walk(path, dir, name); }

long vfs_create(const char *path, uint32_t mode, struct inode **out)
{
    struct inode *dir, *ino;
    char name[NAME_MAX + 1];
    long r = vfs_lookup_parent(path, &dir, name);
    if (r < 0)
        return r;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        r = -EEXIST;
    else
        r = dir->ops->create(dir, name, mode, &ino);
    iput(dir);
    if (r == 0) {
        iget(ino);
        *out = ino;
    }
    return r;
}

struct file *file_new(struct inode *ino, int flags)
{
    struct file *f = kmalloc(sizeof *f);
    if (f) {
        iget(ino);
        f->ino = ino;
        f->flags = flags;
        f->refs = 1;
    }
    return f;
}

void file_put(struct file *f)
{
    if (--f->refs)
        return;
    if (f->ino->ops->close)
        f->ino->ops->close(f->ino, f->flags);
    iput(f->ino);
    kfree(f);
}

// Copy an embedded program into the tree.
static void install(const char *path, const uint8_t *data, size_t size)
{
    struct inode *ino;
    if (vfs_create(path, S_IFREG | 0755, &ino) < 0 ||
        ino->ops->write(ino, (const char *)data, size, 0) != (long)size)
        panic("vfs: cannot install %s", path);
    iput(ino);
}

void vfs_init(void)
{
    static const char *const dirs[] = {
        "/bin", "/dev", "/etc", "/home", "/proc", "/sbin", "/tmp", "/usr", "/var",
    };
    root = ramfs_new_root();
    struct inode *d;
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        if (vfs_create(dirs[i], S_IFDIR | 0755, &d) < 0)
            panic("vfs: mkdir %s failed", dirs[i]);
        iput(d);
    }
    if (vfs_lookup("/dev", &d) < 0)
        panic("vfs: no /dev");
    devfs_init(d);
    iput(d);

    int n = 0;
    for (const struct initrd_file *f = initrd_table; f->name; f++, n++)
        install(f->name, f->start, (size_t)(f->end - f->start));
    kprintf("[VFS] ramfs root, /dev/console, /dev/null, %d programs installed\n", n);
    fat_mount_boot();
}
