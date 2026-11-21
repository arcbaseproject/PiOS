// Device nodes in /dev: console (UART, cooked line input) and null.
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "sched.h"
#include "uart.h"
#include "vfs.h"

// ---- console line discipline ----
// IRQ side edits the current line (echo, backspace, ^C discards, ^D flushes);
// completed lines move to `ready`, which read() drains.

#define LINE_MAX 256
#define READY_MAX 1024

static char line[LINE_MAX];
static size_t line_len;
static char ready[READY_MAX];
static size_t ready_head, ready_count;
static int eof_pending;
static int console_chan;    // sleep channel
static int fg_pgid;         // ^C target group; 0 = none
static int raw;             // !ICANON: bytes pass straight to read(), no echo

// Terminal ioctls. Only ICANON matters: without it the console is raw (every
// byte, ^C included, goes to read() unechoed), like cfmakeraw.
static long console_ioctl(struct inode *ino, unsigned long req, uint64_t uarg)
{
    (void)ino;
    struct task *t = current_task();
    switch (req) {
    case TCGETS: {
        if (!vm_check(t->pgd, uarg, sizeof(struct ktermios), 1))
            return -EFAULT;
        struct ktermios *k = (struct ktermios *)uarg;
        memset(k, 0, sizeof *k);
        k->c_iflag = 0x100;                         // ICRNL
        k->c_oflag = 0x5;                           // OPOST | ONLCR
        k->c_cflag = 0x10b2;                        // B115200 | CS8 | CREAD
        k->c_lflag = raw ? 0 : ISIG | ICANON | ECHO | 0x10 | 0x20;   // + ECHOE ECHOK
        k->c_cc[0] = 3, k->c_cc[2] = 0x7f, k->c_cc[4] = 4, k->c_cc[6] = 1;  // VINTR VERASE VEOF VMIN
        return 0;
    }
    case TCSETS: case TCSETSW: case TCSETSF: {
        if (!vm_check(t->pgd, uarg, sizeof(struct ktermios), 0))
            return -EFAULT;
        raw = !(((struct ktermios *)uarg)->c_lflag & ICANON);
        line_len = 0;
        return 0;
    }
    case TIOCGPGRP:
        if (!vm_check(t->pgd, uarg, sizeof(int), 1))
            return -EFAULT;
        *(int *)uarg = fg_pgid ? fg_pgid : t->pgid;
        return 0;
    case TIOCSPGRP:
        if (!vm_check(t->pgd, uarg, sizeof(int), 0))
            return -EFAULT;
        if (*(int *)uarg < 0)
            return -EINVAL;
        fg_pgid = *(int *)uarg;
        return 0;
    case TIOCGWINSZ: {
        if (!vm_check(t->pgd, uarg, sizeof(struct winsize), 1))
            return -EFAULT;
        // shortcut: fixed 24x80 (a serial terminal cannot be asked cheaply)
        *(struct winsize *)uarg = (struct winsize){ 24, 80, 0, 0 };
        return 0;
    }
    }
    return -ENOTTY;
}

static void ready_push(const char *s, size_t n)
{
    for (size_t i = 0; i < n && ready_count < READY_MAX; i++, ready_count++)
        ready[(ready_head + ready_count) % READY_MAX] = s[i];
}

void console_input(char c)
{
    if (raw) {                  // ^C too: a raw program (editor) decides what it means
        ready_push(&c, 1);
    } else if (c == '\r' || c == '\n') {
        uart_puts("\n");
        line[line_len++] = '\n';
        ready_push(line, line_len);
        line_len = 0;
    } else if (c == 0x7f || c == '\b') {
        if (line_len) {
            line_len--;
            uart_puts("\b \b");
        }
    } else if (c == 3) {            // ^C: discard the line, kill the foreground group
        uart_puts("^C\n");
        line_len = 0;
        if (fg_pgid)
            kill_pgrp(fg_pgid, SIGINT);
    } else if (c == 4) {            // ^D: flush line; on empty line, EOF
        if (line_len)
            ready_push(line, line_len);
        else
            eof_pending++;
        line_len = 0;
    } else if (c >= 0x20 && line_len < LINE_MAX - 1) {
        uart_putc(c);
        line[line_len++] = c;
    } else {
        return;
    }
    wakeup(&console_chan);
}

static long console_read(struct inode *ino, char *buf, size_t len, uint64_t off)
{
    (void)ino, (void)off;
    while (ready_count == 0 && !eof_pending)
        if (sleep_on(&console_chan))
            return -EINTR;
    if (ready_count == 0) {
        eof_pending--;
        return 0;
    }
    size_t n = 0;
    while (n < len && ready_count) {
        char c = ready[ready_head];
        ready_head = (ready_head + 1) % READY_MAX;
        ready_count--;
        buf[n++] = c;
        if (c == '\n' && !raw)
            break;      // one line per read, like a cooked tty
    }
    return (long)n;
}

static long console_write(struct inode *ino, const char *buf, size_t len, uint64_t off)
{
    (void)ino, (void)off;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n')
            uart_putc('\r');
        uart_putc(buf[i]);
    }
    return (long)len;
}

static int console_poll(struct inode *ino)
{
    (void)ino;
    return (ready_count || eof_pending ? POLLIN : 0) | POLLOUT;
}

static const struct inode_ops console_ops = { .read = console_read, .write = console_write,
                                              .ioctl = console_ioctl, .poll = console_poll };

// ---- null ----

static long null_read(struct inode *i, char *b, size_t l, uint64_t o)
{
    (void)i, (void)b, (void)l, (void)o;
    return 0;
}

static long null_write(struct inode *i, const char *b, size_t l, uint64_t o)
{
    (void)i, (void)b, (void)o;
    return (long)l;
}

static const struct inode_ops null_ops = { .read = null_read, .write = null_write };

void devfs_init(struct inode *dev)
{
    struct inode *con = inode_new(S_IFCHR | 0666, &console_ops);
    struct inode *nul = inode_new(S_IFCHR | 0666, &null_ops);
    if (!con || !nul || dev->ops->link(dev, "console", con) < 0 ||
        dev->ops->link(dev, "null", nul) < 0)
        panic("devfs: cannot create device nodes");
    uart_enable_rx_irq();
}
