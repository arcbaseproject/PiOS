// BSD sockets (Linux ABI) over lwIP's raw API: AF_INET TCP, UDP and raw
// ICMP (for ping). A socket is an inode with S_IFSOCK, so read/write/close/
// ppoll on its fd work. Blocking calls sleep on the socket; lwIP callbacks
// (run from the USB thread's net_input) wake them. All lwIP calls happen with
// IRQs masked (syscalls run that way; see NET_LOCK in net.h).
// shortcut: no AF_UNIX, no SO_RCVTIMEO/SO_SNDTIMEO (ignored), sendmsg/recvmsg
// with one iovec (several only for TCP sends), no IP_HDRINCL, no ancillary data.
#include "kernel.h"
#include "mm.h"
#include "net.h"
#include "proc.h"
#include "sched.h"
#include "vfs.h"
#include "lwip/ip_addr.h"
#include "lwip/raw.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"

#define BACKLOG_MAX 16
#define DGRAM_MAX   64

struct dgram {
    struct dgram *next;
    struct pbuf *p;
    ip_addr_t addr;
    u16_t port;
};

struct sock {
    int type, nonblock, err, eof, connecting, connected, listening;
    struct tcp_pcb *tcp;
    struct udp_pcb *udp;
    struct raw_pcb *raw;
    struct pbuf *rx;                    // TCP: received, not yet read
    struct dgram *dq, *dtail;           // UDP/raw: queued datagrams
    int dqn;
    struct sock *acc[BACKLOG_MAX];      // listening: accepted, not yet accept()ed
    int nacc;
};

static int errno_of(err_t e)
{
    switch (e) {
    case ERR_OK: return 0;
    case ERR_MEM: return ENOMEM;
    case ERR_BUF: return ENOBUFS;
    case ERR_TIMEOUT: return ETIMEDOUT;
    case ERR_RTE: return EHOSTUNREACH;
    case ERR_INPROGRESS: return EINPROGRESS;
    case ERR_WOULDBLOCK: return EAGAIN;
    case ERR_USE: return EADDRINUSE;
    case ERR_ALREADY: return EALREADY;
    case ERR_ISCONN: return EISCONN;
    case ERR_CONN: return ENOTCONN;
    case ERR_ABRT: return ECONNABORTED;
    case ERR_RST: return ECONNRESET;
    case ERR_CLSD: return ENOTCONN;
    case ERR_IF: return EIO;
    default: return EINVAL;
    }
}

static int wait(struct sock *s) { return sleep_on(s); }    // -EINTR on a signal

// ---- lwIP callbacks ----

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct sock *s = arg;
    (void)err;
    if (!s) {
        if (p) {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }
        return ERR_OK;
    }
    if (!p)
        s->eof = 1;
    else if (s->rx)
        pbuf_cat(s->rx, p);
    else
        s->rx = p;
    wakeup(s);
    return ERR_OK;
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb, (void)len;
    if (arg)
        wakeup(arg);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)        // the pcb is already freed
{
    struct sock *s = arg;
    if (!s)
        return;
    s->tcp = NULL;
    s->err = s->connecting && err == ERR_RST ? ECONNREFUSED : errno_of(err);
    s->connecting = 0;
    s->eof = 1;
    wakeup(s);
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)pcb;
    struct sock *s = arg;
    if (s) {
        s->connecting = 0;
        s->connected = err == ERR_OK;
        s->err = errno_of(err);
        wakeup(s);
    }
    return ERR_OK;
}

static void tcp_setup(struct sock *s, struct tcp_pcb *pcb)
{
    s->tcp = pcb;
    tcp_arg(pcb, s);
    tcp_recv(pcb, tcp_recv_cb);
    tcp_sent(pcb, tcp_sent_cb);
    tcp_err(pcb, tcp_err_cb);
}

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    struct sock *s = arg, *ns;
    if (!s || err != ERR_OK || s->nacc >= BACKLOG_MAX || !(ns = kmalloc(sizeof *ns)))
        return ERR_MEM;                 // lwIP aborts the new connection
    ns->type = SOCK_STREAM;
    ns->connected = 1;
    tcp_setup(ns, newpcb);
    s->acc[s->nacc++] = ns;
    wakeup(s);
    return ERR_OK;
}

static void enqueue(struct sock *s, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    struct dgram *d = s->dqn < DGRAM_MAX ? kmalloc(sizeof *d) : NULL;
    if (!d) {
        pbuf_free(p);
        return;
    }
    d->p = p;
    d->addr = *addr;
    d->port = port;
    if (s->dtail)
        s->dtail->next = d;
    else
        s->dq = d;
    s->dtail = d;
    s->dqn++;
    wakeup(s);
}

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)pcb;
    enqueue(arg, p, addr, port);
}

static u8_t raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    (void)pcb;
    struct pbuf *c = pbuf_clone(PBUF_RAW, PBUF_RAM, p);     // lwIP keeps processing p (ICMP echo)
    if (c)
        enqueue(arg, c, addr, 0);
    return 0;
}

// ---- file operations on a socket inode ----

static void drop_dgrams(struct sock *s)
{
    while (s->dq) {
        struct dgram *d = s->dq;
        s->dq = d->next;
        pbuf_free(d->p);
        kfree(d);
    }
    s->dtail = NULL;
    s->dqn = 0;
}

static void tcp_teardown(struct sock *s)
{
    if (s->tcp) {
        struct tcp_pcb *pcb = s->tcp;
        s->tcp = NULL;
        tcp_arg(pcb, NULL);
        if (!s->listening) {
            tcp_recv(pcb, NULL);
            tcp_sent(pcb, NULL);
            tcp_err(pcb, NULL);
        }
        if (s->rx || tcp_close(pcb) != ERR_OK)
            tcp_abort(pcb);             // unread data: reset, like Linux
    }
    if (s->rx) {
        pbuf_free(s->rx);
        s->rx = NULL;
    }
}

static void sock_close(struct inode *ino, int flags)
{
    (void)flags;
    struct sock *s = ino->priv;
    NET_LOCK();
    for (int i = 0; i < s->nacc; i++) {
        tcp_teardown(s->acc[i]);
        kfree(s->acc[i]);
    }
    s->nacc = 0;
    tcp_teardown(s);
    if (s->udp)
        udp_remove(s->udp);
    if (s->raw)
        raw_remove(s->raw);
    s->udp = NULL, s->raw = NULL;
    drop_dgrams(s);
    NET_UNLOCK();
}

static void sock_release(struct inode *ino) { kfree(ino->priv); }

static long tcp_read(struct sock *s, char *buf, size_t len, int flags)
{
    while (!s->rx) {
        if (s->eof || !s->tcp)
            return s->err && s->err != ENOTCONN && !s->eof ? -s->err : 0;
        if (!s->connected && !s->connecting)
            return -ENOTCONN;
        if (s->nonblock || (flags & MSG_DONTWAIT))
            return -EAGAIN;
        if (wait(s))
            return -EINTR;
    }
    size_t n = len < s->rx->tot_len ? len : s->rx->tot_len;
    pbuf_copy_partial(s->rx, buf, (u16_t)n, 0);
    if (!(flags & MSG_PEEK)) {
        s->rx = pbuf_free_header(s->rx, (u16_t)n);
        if (s->tcp)
            tcp_recved(s->tcp, (u16_t)n);
    }
    return (long)n;
}

static long tcp_send(struct sock *s, const char *buf, size_t len, int flags)
{
    size_t sent = 0;
    while (sent < len) {
        if (!s->tcp || s->eof) {
            if (!(flags & MSG_NOSIGNAL))
                sys_kill(current_task()->pid, SIGPIPE);
            return sent ? (long)sent : -EPIPE;
        }
        u16_t room = tcp_sndbuf(s->tcp);
        size_t chunk = len - sent < room ? len - sent : room;
        err_t e = chunk ? tcp_write(s->tcp, buf + sent, (u16_t)chunk, TCP_WRITE_FLAG_COPY) : ERR_MEM;
        if (e == ERR_OK) {
            sent += chunk;
            continue;
        }
        if (e != ERR_MEM)
            return sent ? (long)sent : -errno_of(e);
        tcp_output(s->tcp);
        if (s->nonblock || (flags & MSG_DONTWAIT))
            return sent ? (long)sent : -EAGAIN;
        if (wait(s))
            return sent ? (long)sent : -EINTR;
    }
    if (s->tcp)
        tcp_output(s->tcp);
    return (long)sent;
}

// Datagram receive; fills *from (if not NULL). Returns bytes copied.
static long dgram_read(struct sock *s, char *buf, size_t len, int flags, struct sockaddr_in *from)
{
    while (!s->dq) {
        if (s->nonblock || (flags & MSG_DONTWAIT))
            return -EAGAIN;
        if (wait(s))
            return -EINTR;
    }
    struct dgram *d = s->dq;
    size_t n = len < d->p->tot_len ? len : d->p->tot_len;
    pbuf_copy_partial(d->p, buf, (u16_t)n, 0);
    if (from)
        *from = (struct sockaddr_in){ AF_INET, lwip_htons(d->port), ip4_addr_get_u32(ip_2_ip4(&d->addr)), {0} };
    if (!(flags & MSG_PEEK)) {
        s->dq = d->next;
        if (!s->dq)
            s->dtail = NULL;
        s->dqn--;
        pbuf_free(d->p);
        kfree(d);
    }
    return (long)n;
}

static long dgram_send(struct sock *s, const char *buf, size_t len, const ip_addr_t *to, u16_t port)
{
    if (len > 65000)
        return -EMSGSIZE;
    struct pbuf *p = pbuf_alloc(s->raw ? PBUF_IP : PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p)
        return -ENOBUFS;
    memcpy(p->payload, buf, len);
    err_t e;
    if (s->udp)
        e = to ? udp_sendto(s->udp, p, to, port) : udp_send(s->udp, p);
    else
        e = to ? raw_sendto(s->raw, p, to) : raw_send(s->raw, p);
    pbuf_free(p);
    return e == ERR_OK ? (long)len : -errno_of(e);
}

static long sock_read(struct inode *ino, char *buf, size_t len, uint64_t off)
{
    (void)off;
    struct sock *s = ino->priv;
    NET_LOCK();
    long r = s->type == SOCK_STREAM ? tcp_read(s, buf, len, 0) : dgram_read(s, buf, len, 0, NULL);
    NET_UNLOCK();
    return r;
}

static long sock_write(struct inode *ino, const char *buf, size_t len, uint64_t off)
{
    (void)off;
    struct sock *s = ino->priv;
    NET_LOCK();
    long r;
    if (s->type == SOCK_STREAM)
        r = tcp_send(s, buf, len, 0);
    else if ((s->udp && ip_addr_isany(&s->udp->remote_ip)) || (s->raw && ip_addr_isany(&s->raw->remote_ip)))
        r = -EDESTADDRREQ;
    else
        r = dgram_send(s, buf, len, NULL, 0);
    NET_UNLOCK();
    return r;
}

static int sock_poll(struct inode *ino)
{
    struct sock *s = ino->priv;
    if (s->type != SOCK_STREAM)
        return (s->dq ? POLLIN : 0) | POLLOUT;
    if (s->listening)
        return s->nacc ? POLLIN : 0;
    int r = 0;
    if (s->rx || s->eof || s->err)
        r |= POLLIN;
    if (s->err || (s->connected && s->tcp && tcp_sndbuf(s->tcp) > 0))
        r |= POLLOUT;
    if (s->eof && !s->tcp)
        r |= POLLHUP;
    if (s->err)
        r |= POLLERR;
    return r;
}

static long sock_ioctl(struct inode *ino, unsigned long req, uint64_t uarg)
{
    struct sock *s = ino->priv;
    if (!vm_check(current_task()->pgd, uarg, sizeof(int), req == FIONREAD))
        return -EFAULT;
    if (req == FIONREAD) {
        *(int *)uarg = s->type == SOCK_STREAM ? (s->rx ? s->rx->tot_len : 0) : (s->dq ? s->dq->p->tot_len : 0);
        return 0;
    }
    if (req == FIONBIO) {
        s->nonblock = *(int *)uarg != 0;
        return 0;
    }
    return -ENOTTY;
}

static const struct inode_ops sock_ops = {
    .read = sock_read, .write = sock_write, .close = sock_close, .release = sock_release,
    .poll = sock_poll, .ioctl = sock_ioctl,
};

void sock_set_nonblock(struct inode *ino, int on) { ((struct sock *)ino->priv)->nonblock = on; }

// ---- syscalls ----

static struct sock *sock_of(int fd, long *err)
{
    struct file *f = getf(fd);
    *err = !f ? -EBADF : !S_ISSOCK(f->ino->mode) ? -ENOTSOCK : 0;
    return *err ? NULL : f->ino->priv;
}

static int install(struct sock *s, int flags)
{
    struct inode *ino = inode_new(S_IFSOCK | 0777, &sock_ops);
    if (!ino)
        return -ENOMEM;
    ino->priv = s;
    s->nonblock = (flags & SOCK_NONBLOCK) != 0;
    struct file *f = file_new(ino, O_RDWR | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0));
    if (!f) {
        kfree(ino);
        return -ENOMEM;
    }
    int fd = fd_install(f);
    if (fd < 0)
        file_put(f);                    // closes and frees the socket
    else
        current_task()->cloexec[fd] = (flags & SOCK_CLOEXEC) != 0;
    return fd;
}

static long get_addr(uint64_t uaddr, uint64_t len, ip_addr_t *ip, u16_t *port)
{
    if (len < sizeof(struct sockaddr_in) || !vm_check(current_task()->pgd, uaddr, sizeof(struct sockaddr_in), 0))
        return len < sizeof(struct sockaddr_in) ? -EINVAL : -EFAULT;
    const struct sockaddr_in *sa = (const struct sockaddr_in *)uaddr;
    if (sa->sin_family != AF_INET)
        return -EAFNOSUPPORT;
    ip_addr_set_ip4_u32(ip, sa->sin_addr);
    *port = lwip_ntohs(sa->sin_port);
    return 0;
}

// Copy a sockaddr_in out to (uaddr, *ulen), Linux style (truncate, report full length).
static long put_addr(uint64_t uaddr, uint64_t ulen, const ip_addr_t *ip, u16_t port)
{
    if (!uaddr)
        return 0;
    struct task *t = current_task();
    if (!vm_check(t->pgd, ulen, sizeof(uint32_t), 1))
        return -EFAULT;
    uint32_t have = *(uint32_t *)ulen;
    struct sockaddr_in sa = { AF_INET, lwip_htons(port), ip ? ip4_addr_get_u32(ip_2_ip4(ip)) : 0, {0} };
    uint32_t n = have < sizeof sa ? have : sizeof sa;
    if (!vm_check(t->pgd, uaddr, n, 1))
        return -EFAULT;
    memcpy((void *)uaddr, &sa, n);
    *(uint32_t *)ulen = sizeof sa;
    return 0;
}

long sys_socket(int domain, int type, int proto)
{
    if (domain != AF_INET)
        return -EAFNOSUPPORT;
    int base = type & 0xf;
    struct sock *s = kmalloc(sizeof *s);
    if (!s)
        return -ENOMEM;
    s->type = base;
    NET_LOCK();
    long r = 0;
    if (base == SOCK_STREAM && (proto == 0 || proto == IPPROTO_TCP)) {
        struct tcp_pcb *pcb = tcp_new();
        if (pcb)
            tcp_setup(s, pcb);
        else
            r = -ENOMEM;
    } else if (base == SOCK_DGRAM && (proto == 0 || proto == IPPROTO_UDP)) {
        if ((s->udp = udp_new())) {
            udp_recv(s->udp, udp_recv_cb, s);
            ip_set_option(s->udp, SOF_BROADCAST);
        } else {
            r = -ENOMEM;
        }
    } else if (base == SOCK_RAW && proto == IPPROTO_ICMP) {
        if ((s->raw = raw_new(IP_PROTO_ICMP)))
            raw_recv(s->raw, raw_recv_cb, s);
        else
            r = -ENOMEM;
    } else {
        r = -EPROTONOSUPPORT;
    }
    NET_UNLOCK();
    if (r < 0) {
        kfree(s);
        return r;
    }
    return install(s, type);
}

long sys_bind(int fd, uint64_t uaddr, uint64_t len)
{
    long r;
    struct sock *s = sock_of(fd, &r);
    ip_addr_t ip;
    u16_t port;
    if (!s || (r = get_addr(uaddr, len, &ip, &port)) < 0)
        return r;
    NET_LOCK();
    err_t e = s->tcp ? tcp_bind(s->tcp, &ip, port) : s->udp ? udp_bind(s->udp, &ip, port) :
              s->raw ? raw_bind(s->raw, &ip) : ERR_CLSD;
    NET_UNLOCK();
    return -errno_of(e);
}

long sys_listen(int fd, int backlog)
{
    long r;
    struct sock *s = sock_of(fd, &r);
    if (!s)
        return r;
    if (s->type != SOCK_STREAM || !s->tcp)
        return -EOPNOTSUPP;
    if (s->listening)
        return 0;
    NET_LOCK();
    if (s->tcp->local_port == 0)
        tcp_bind(s->tcp, IP4_ADDR_ANY, 0);
    struct tcp_pcb *l = tcp_listen_with_backlog(s->tcp, (u8_t)(backlog < 1 ? 1 : backlog > BACKLOG_MAX ? BACKLOG_MAX : backlog));
    if (l) {
        s->tcp = l;
        s->listening = 1;
        tcp_arg(l, s);
        tcp_accept(l, tcp_accept_cb);
    }
    NET_UNLOCK();
    return l ? 0 : -ENOMEM;
}

long sys_accept4(int fd, uint64_t uaddr, uint64_t ulen, int flags)
{
    long r;
    struct sock *s = sock_of(fd, &r);
    if (!s)
        return r;
    if (!s->listening)
        return -EINVAL;
    NET_LOCK();
    while (!s->nacc) {
        if (s->nonblock || !s->tcp) {
            NET_UNLOCK();
            return s->tcp ? -EAGAIN : -EINVAL;
        }
        if (wait(s)) {
            NET_UNLOCK();
            return -EINTR;
        }
    }
    struct sock *ns = s->acc[0];
    memmove(s->acc, s->acc + 1, (size_t)--s->nacc * sizeof *s->acc);
    ip_addr_t peer = ns->tcp ? ns->tcp->remote_ip : *IP4_ADDR_ANY;
    u16_t port = ns->tcp ? ns->tcp->remote_port : 0;
    NET_UNLOCK();
    int nfd = install(ns, flags);
    if (nfd >= 0)
        put_addr(uaddr, ulen, &peer, port);
    return nfd;
}

long sys_connect(int fd, uint64_t uaddr, uint64_t len)
{
    long r;
    struct sock *s = sock_of(fd, &r);
    ip_addr_t ip;
    u16_t port;
    if (!s || (r = get_addr(uaddr, len, &ip, &port)) < 0)
        return r;
    NET_LOCK();
    if (s->udp)
        r = -errno_of(udp_connect(s->udp, &ip, port));
    else if (s->raw)
        r = -errno_of(raw_connect(s->raw, &ip));
    else if (s->connected)
        r = -EISCONN;
    else if (s->connecting)
        r = -EALREADY;
    else if (!s->tcp)
        r = -EINVAL;
    else if ((r = -errno_of(tcp_connect(s->tcp, &ip, port, tcp_connected_cb))) == 0) {
        s->connecting = 1;
        if (s->nonblock)
            r = -EINPROGRESS;
        else
            while (s->connecting)
                if (wait(s)) {
                    r = -EINTR;
                    break;
                }
        if (r == 0 && !s->connected)
            r = -(s->err ? s->err : ECONNREFUSED);
    }
    NET_UNLOCK();
    return r;
}

long sys_getsockname(int fd, uint64_t uaddr, uint64_t ulen, int peer)
{
    long r;
    struct sock *s = sock_of(fd, &r);
    if (!s)
        return r;
    NET_LOCK();
    ip_addr_t ip = *IP4_ADDR_ANY;
    u16_t port = 0;
    if (s->tcp)
        ip = peer ? s->tcp->remote_ip : s->tcp->local_ip, port = peer ? s->tcp->remote_port : s->tcp->local_port;
    else if (s->udp)
        ip = peer ? s->udp->remote_ip : s->udp->local_ip, port = peer ? s->udp->remote_port : s->udp->local_port;
    if (peer && !(s->connected || (s->udp && port)))
        r = -ENOTCONN;
    NET_UNLOCK();
    return r < 0 ? r : put_addr(uaddr, ulen, &ip, port);
}

long sys_sendto(int fd, uint64_t ubuf, uint64_t len, int flags, uint64_t uaddr, uint64_t alen)
{
    long r;
    struct sock *s = sock_of(fd, &r);
    if (!s)
        return r;
    if (!vm_check(current_task()->pgd, ubuf, len, 0))
        return -EFAULT;
    ip_addr_t ip;
    u16_t port = 0;
    int to = s->type != SOCK_STREAM && uaddr;
    if (to && (r = get_addr(uaddr, alen, &ip, &port)) < 0)
        return r;
    NET_LOCK();
    if (s->type == SOCK_STREAM)
        r = tcp_send(s, (const char *)ubuf, len, flags);
    else if (!to && ((s->udp && ip_addr_isany(&s->udp->remote_ip)) || (s->raw && ip_addr_isany(&s->raw->remote_ip))))
        r = -EDESTADDRREQ;
    else
        r = dgram_send(s, (const char *)ubuf, len, to ? &ip : NULL, port);
    NET_UNLOCK();
    return r;
}

long sys_recvfrom(int fd, uint64_t ubuf, uint64_t len, int flags, uint64_t uaddr, uint64_t ulen)
{
    long r;
    struct sock *s = sock_of(fd, &r);
    if (!s)
        return r;
    if (!vm_check(current_task()->pgd, ubuf, len, 1))
        return -EFAULT;
    struct sockaddr_in from = { 0 };
    NET_LOCK();
    if (s->type == SOCK_STREAM) {
        r = tcp_read(s, (char *)ubuf, len, flags);
        if (s->tcp)
            from = (struct sockaddr_in){ AF_INET, lwip_htons(s->tcp->remote_port),
                                         ip4_addr_get_u32(ip_2_ip4(&s->tcp->remote_ip)), {0} };
    } else {
        r = dgram_read(s, (char *)ubuf, len, flags, &from);
    }
    NET_UNLOCK();
    if (r >= 0 && uaddr) {
        ip_addr_t ip;
        ip_addr_set_ip4_u32(&ip, from.sin_addr);
        long e = put_addr(uaddr, ulen, &ip, lwip_ntohs(from.sin_port));
        if (e < 0)
            return e;
    }
    return r;
}

long sys_sendmsg(int fd, uint64_t umsg, int flags)
{
    struct task *t = current_task();
    if (!vm_check(t->pgd, umsg, sizeof(struct msghdr), 0))
        return -EFAULT;
    const struct msghdr *m = (const struct msghdr *)umsg;
    if (m->msg_iovlen > 64 || !vm_check(t->pgd, m->msg_iov, m->msg_iovlen * sizeof(struct iovec), 0))
        return -EFAULT;
    const struct iovec *iov = (const struct iovec *)m->msg_iov;
    if (m->msg_iovlen == 1)
        return sys_sendto(fd, iov[0].iov_base, iov[0].iov_len, flags, m->msg_name, m->msg_namelen);
    long r, total = 0;
    struct sock *s = sock_of(fd, &r);
    if (!s)
        return r;
    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;     // shortcut: scatter for datagrams
    for (uint64_t i = 0; i < m->msg_iovlen; i++) {
        r = sys_sendto(fd, iov[i].iov_base, iov[i].iov_len, flags, 0, 0);
        if (r < 0)
            return total ? total : r;
        total += r;
    }
    return total;
}

long sys_recvmsg(int fd, uint64_t umsg, int flags)
{
    struct task *t = current_task();
    if (!vm_check(t->pgd, umsg, sizeof(struct msghdr), 1))
        return -EFAULT;
    struct msghdr *m = (struct msghdr *)umsg;
    if (m->msg_iovlen < 1 || !vm_check(t->pgd, m->msg_iov, sizeof(struct iovec), 0))
        return -EINVAL;
    const struct iovec *iov = (const struct iovec *)m->msg_iov;
    uint64_t lenp = umsg + __builtin_offsetof(struct msghdr, msg_namelen);
    long r = sys_recvfrom(fd, iov[0].iov_base, iov[0].iov_len, flags, m->msg_name, m->msg_name ? lenp : 0);
    if (r >= 0) {
        m->msg_controllen = 0;
        m->msg_flags = 0;
    }
    return r;
}

long sys_setsockopt(int fd, int level, int opt, uint64_t uval, uint64_t len)
{
    long r;
    struct sock *s = sock_of(fd, &r);
    if (!s)
        return r;
    int v = len >= sizeof(int) && vm_check(current_task()->pgd, uval, sizeof(int), 0) ? *(int *)uval : 0;
    NET_LOCK();
    if (level == SOL_SOCKET && (opt == SO_REUSEADDR || opt == SO_KEEPALIVE || opt == SO_BROADCAST)) {
        u8_t f = opt == SO_REUSEADDR ? SOF_REUSEADDR : opt == SO_KEEPALIVE ? SOF_KEEPALIVE : SOF_BROADCAST;
        if (s->tcp && v)
            ip_set_option(s->tcp, f);
        else if (s->tcp)
            ip_reset_option(s->tcp, f);
        else if (s->udp && v)
            ip_set_option(s->udp, f);
        else if (s->udp)
            ip_reset_option(s->udp, f);
    } else if (level == IPPROTO_TCP && opt == TCP_NODELAY && s->tcp && !s->listening) {
        if (v)
            tcp_nagle_disable(s->tcp);
        else
            tcp_nagle_enable(s->tcp);
    }
    NET_UNLOCK();
    return 0;                   // other options are accepted and ignored
}

long sys_getsockopt(int fd, int level, int opt, uint64_t uval, uint64_t ulen)
{
    long r;
    struct sock *s = sock_of(fd, &r);
    if (!s)
        return r;
    struct task *t = current_task();
    if (!vm_check(t->pgd, ulen, sizeof(uint32_t), 1) || *(uint32_t *)ulen < sizeof(int) ||
        !vm_check(t->pgd, uval, sizeof(int), 1))
        return -EFAULT;
    int v = 0;
    if (level == SOL_SOCKET && opt == SO_ERROR)
        v = s->err, s->err = 0;
    else if (level == SOL_SOCKET && opt == SO_TYPE)
        v = s->type;
    *(int *)uval = v;
    *(uint32_t *)ulen = sizeof(int);
    return 0;
}

long sys_shutdown(int fd, int how)
{
    long r;
    struct sock *s = sock_of(fd, &r);
    if (!s)
        return r;
    if (s->type != SOCK_STREAM)
        return 0;
    if (!s->tcp || s->listening)
        return -ENOTCONN;
    if (how == SHUT_RD)
        return 0;               // shortcut: receive side stays open (lwIP would free the pcb)
    NET_LOCK();
    r = -errno_of(tcp_shutdown(s->tcp, 0, 1));
    NET_UNLOCK();
    return r;
}
