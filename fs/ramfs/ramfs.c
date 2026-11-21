// In-memory filesystem. Files: one index page of 512 data-page pointers
// (max 2 MiB per file). Directories: singly linked list of entries.
#include "kernel.h"
#include "mm.h"
#include "vfs.h"

#define MAX_PAGES (PAGE_SIZE / sizeof(void *))
#define MAX_FILE  (MAX_PAGES * PAGE_SIZE)

struct rdent {
    char name[NAME_MAX + 1];
    struct inode *ino;
    struct rdent *next;
};

static const struct inode_ops file_ops, dir_ops;

// ---- regular files ----

static long file_read(struct inode *ino, char *buf, size_t len, uint64_t off)
{
    void **idx = ino->priv;
    if (off >= ino->size)
        return 0;
    if (len > ino->size - off)
        len = ino->size - off;
    for (size_t done = 0; done < len; ) {
        uint64_t pos = off + done;
        size_t n = PAGE_SIZE - pos % PAGE_SIZE;
        if (n > len - done)
            n = len - done;
        void *pg = idx ? idx[pos / PAGE_SIZE] : NULL;
        if (pg)
            memcpy(buf + done, (char *)pg + pos % PAGE_SIZE, n);
        else
            memset(buf + done, 0, n);     // hole
        done += n;
    }
    return (long)len;
}

static long file_write(struct inode *ino, const char *buf, size_t len, uint64_t off)
{
    if (off > MAX_FILE || len > MAX_FILE - off)
        return -EFBIG;
    if (!ino->priv && !(ino->priv = page_alloc()))
        return -ENOSPC;
    void **idx = ino->priv;
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        size_t n = PAGE_SIZE - pos % PAGE_SIZE;
        if (n > len - done)
            n = len - done;
        void **pg = &idx[pos / PAGE_SIZE];
        if (!*pg && !(*pg = page_alloc()))
            break;
        memcpy((char *)*pg + pos % PAGE_SIZE, buf + done, n);
        done += n;
    }
    if (off + done > ino->size)
        ino->size = off + done;
    return done ? (long)done : -ENOSPC;
}

static void file_truncate(struct inode *ino)
{
    void **idx = ino->priv;
    if (idx) {
        for (size_t i = 0; i < MAX_PAGES; i++)
            if (idx[i])
                page_free(idx[i]);
        page_free(idx);
    }
    ino->priv = NULL;
    ino->size = 0;
}

static const struct inode_ops file_ops = {
    .read = file_read, .write = file_write,
    .truncate = file_truncate, .release = file_truncate,
};

// ---- directories ----

static struct rdent *find(struct inode *dir, const char *name, struct rdent ***link)
{
    struct rdent **pp = (struct rdent **)&dir->priv;
    for (; *pp; pp = &(*pp)->next)
        if (strcmp((*pp)->name, name) == 0)
            break;
    if (link)
        *link = pp;
    return *pp;
}

static long dir_lookup(struct inode *dir, const char *name, struct inode **out)
{
    struct rdent *e = find(dir, name, NULL);
    if (!e)
        return -ENOENT;
    *out = e->ino;
    return 0;
}

static long dir_link(struct inode *dir, const char *name, struct inode *ino)
{
    if (dir->nlink == 0)
        return -ENOENT;     // directory was removed
    if (find(dir, name, NULL))
        return -EEXIST;
    struct rdent *e = kmalloc(sizeof *e);
    if (!e)
        return -ENOSPC;
    memcpy(e->name, name, strlen(name) + 1);
    e->ino = ino;
    e->next = dir->priv;
    dir->priv = e;
    dir->size++;
    ino->nlink++;
    if (S_ISDIR(ino->mode))
        ino->parent = dir;
    return 0;
}

static long dir_create(struct inode *dir, const char *name, uint32_t mode, struct inode **out)
{
    if (find(dir, name, NULL))
        return -EEXIST;
    struct inode *ino = inode_new(mode, S_ISDIR(mode) ? &dir_ops : &file_ops);
    if (!ino)
        return -ENOSPC;
    long r = dir_link(dir, name, ino);
    if (r < 0) {
        kfree(ino);
        return r;
    }
    *out = ino;
    return 0;
}

static long dir_unlink(struct inode *dir, const char *name)
{
    struct rdent **pp;
    struct rdent *e = find(dir, name, &pp);
    if (!e)
        return -ENOENT;
    *pp = e->next;
    e->ino->nlink--;
    dir->size--;
    kfree(e);
    return 0;
}

static long dir_readdir(struct inode *dir, uint64_t idx, struct dirent *out)
{
    struct rdent *e = dir->priv;
    while (e && idx--)
        e = e->next;
    if (!e)
        return 0;
    out->d_ino = e->ino->ino;
    out->d_mode = e->ino->mode & S_IFMT;
    memcpy(out->d_name, e->name, strlen(e->name) + 1);
    return 1;
}

static const struct inode_ops dir_ops = {
    .lookup = dir_lookup, .create = dir_create, .link = dir_link,
    .unlink = dir_unlink, .readdir = dir_readdir,
};

struct inode *ramfs_new_root(void)
{
    struct inode *root = inode_new(S_IFDIR | 0755, &dir_ops);
    if (!root)
        panic("ramfs: no memory for root");
    root->nlink = 1;
    root->refs = 1;         // pinned forever
    root->parent = root;
    return root;
}
