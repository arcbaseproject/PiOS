// Anonymous pipes: one-page ring buffer, blocking read/write.
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "sched.h"
#include "vfs.h"

struct pipe {
    char *buf;
    size_t head, count;
    int readers, writers;
};

static long pipe_read(struct inode *ino, char *buf, size_t len, uint64_t off)
{
    (void)off;
    struct pipe *p = ino->priv;
    while (p->count == 0) {
        if (p->writers == 0)
            return 0;               // EOF
        if (sleep_on(p))
            return -EINTR;
    }
    size_t n = 0;
    while (n < len && p->count) {
        buf[n++] = p->buf[p->head];
        p->head = (p->head + 1) % PAGE_SIZE;
        p->count--;
    }
    wakeup(p);
    return (long)n;
}

static long pipe_write(struct inode *ino, const char *buf, size_t len, uint64_t off)
{
    (void)off;
    struct pipe *p = ino->priv;
    size_t n = 0;
    while (n < len) {
        if (p->readers == 0)
            return n ? (long)n : -EPIPE;    // no SIGPIPE yet
        if (p->count == PAGE_SIZE) {
            wakeup(p);
            if (sleep_on(p))
                return -EINTR;
            continue;
        }
        p->buf[(p->head + p->count) % PAGE_SIZE] = buf[n++];
        p->count++;
    }
    wakeup(p);
    return (long)n;
}

static void pipe_close(struct inode *ino, int flags)
{
    struct pipe *p = ino->priv;
    if ((flags & O_ACCMODE) == O_RDONLY)
        p->readers--;
    else
        p->writers--;
    wakeup(p);
}

static void pipe_release(struct inode *ino)
{
    struct pipe *p = ino->priv;
    page_free(p->buf);
    kfree(p);
}

static int pipe_poll(struct inode *ino)
{
    struct pipe *p = ino->priv;
    return (p->count ? POLLIN : 0) | (!p->writers ? POLLIN | POLLHUP : 0) |
           (p->count < PAGE_SIZE ? POLLOUT : 0) | (!p->readers ? POLLERR : 0);
}

static const struct inode_ops pipe_ops = {
    .read = pipe_read, .write = pipe_write, .close = pipe_close, .release = pipe_release,
    .poll = pipe_poll,
};

long sys_pipe2(uint64_t ufds, int flags)
{
    struct task *t = current_task();
    if (!vm_check(t->pgd, ufds, 2 * sizeof(int), 1))
        return -EFAULT;
    int rfd = -1, wfd = -1;
    for (int i = 0; i < NFD && wfd < 0; i++)
        if (!t->fd[i]) {
            if (rfd < 0)
                rfd = i;
            else
                wfd = i;
        }
    if (wfd < 0)
        return -EMFILE;

    struct pipe *p = kmalloc(sizeof *p);
    char *buf = page_alloc();
    struct inode *ino = inode_new(S_IFIFO | 0600, &pipe_ops);
    if (!p || !buf || !ino) {
        kfree(p);
        if (buf)
            page_free(buf);
        kfree(ino);
        return -ENOMEM;
    }
    p->buf = buf;
    p->readers = p->writers = 1;
    ino->priv = p;
    struct file *r = file_new(ino, O_RDONLY);
    struct file *w = file_new(ino, O_WRONLY);
    if (!r || !w)
        panic("pipe: out of memory for file objects");
    t->fd[rfd] = r;
    t->fd[wfd] = w;
    t->cloexec[rfd] = t->cloexec[wfd] = (flags & O_CLOEXEC) != 0;
    ((int *)ufds)[0] = rfd;
    ((int *)ufds)[1] = wfd;
    return 0;
}
