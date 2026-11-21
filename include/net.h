#pragma once
#include <stdint.h>

// Network interface glue (net/net.c). The USB Ethernet driver attaches one
// adapter; frames flow through net_input / the send callback.
void net_init(void);
void net_attach(const uint8_t mac[6], int (*send)(const void *frame, int len));
void net_detach(void);
void net_input(const void *frame, int len);
void net_poll(void);                // lwIP timers and loopback (the "net" thread)

// Sockets (net/socket.c), Linux ABI.
struct inode;
void sock_set_nonblock(struct inode *ino, int on);
long sys_socket(int domain, int type, int proto);
long sys_bind(int fd, uint64_t uaddr, uint64_t len);
long sys_listen(int fd, int backlog);
long sys_accept4(int fd, uint64_t uaddr, uint64_t ulen, int flags);
long sys_connect(int fd, uint64_t uaddr, uint64_t len);
long sys_getsockname(int fd, uint64_t uaddr, uint64_t ulen, int peer);
long sys_sendto(int fd, uint64_t ubuf, uint64_t len, int flags, uint64_t uaddr, uint64_t alen);
long sys_recvfrom(int fd, uint64_t ubuf, uint64_t len, int flags, uint64_t uaddr, uint64_t ulen);
long sys_sendmsg(int fd, uint64_t umsg, int flags);
long sys_recvmsg(int fd, uint64_t umsg, int flags);
long sys_setsockopt(int fd, int level, int opt, uint64_t uval, uint64_t len);
long sys_getsockopt(int fd, int level, int opt, uint64_t uval, uint64_t ulen);
long sys_shutdown(int fd, int how);

// Global network lock: lwIP is single-threaded, so every lwIP call (from the
// USB thread or a syscall) runs with IRQs masked.
#define NET_LOCK()   uint64_t net_daif_ = read_sysreg(daif); irq_disable()
#define NET_UNLOCK() write_sysreg(daif, net_daif_)
