// lwIP configuration for PiOS: IPv4 only, raw API driven by the kernel
// (NO_SYS); the socket layer is PiOS's own (net/socket.c). Every lwIP call
// runs with IRQs masked, which is the lock.
#pragma once

#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0
#define LWIP_SOCKET             0
#define LWIP_NETCONN            0
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_ARP                1
#define LWIP_ETHERNET           1
#define LWIP_ICMP               1
#define LWIP_RAW                1
#define LWIP_UDP                1
#define LWIP_TCP                1
#define LWIP_DHCP               1
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_DNS                1
#define LWIP_NETIF_HOSTNAME     1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_STATS              0
#define LWIP_TIMERS             1
#define ARP_QUEUEING            1     // keep every packet sent while ARP resolves
#define MEMP_NUM_ARP_QUEUE      32
#define LWIP_HAVE_LOOPIF        1     // 127.0.0.1, delivered by net_poll
#define LWIP_NETIF_LOOPBACK     1

#define MEM_ALIGNMENT           8
#define MEM_SIZE                (256 * 1024)
#define MEMP_NUM_PBUF           64
#define PBUF_POOL_SIZE          128
#define MEMP_NUM_RAW_PCB        8
#define MEMP_NUM_UDP_PCB        16
#define MEMP_NUM_TCP_PCB        32
#define MEMP_NUM_TCP_PCB_LISTEN 16
#define MEMP_NUM_TCP_SEG        128
#define TCP_MSS                 1460
#define TCP_WND                 (8 * TCP_MSS)
#define TCP_SND_BUF             (8 * TCP_MSS)
#define TCP_SND_QUEUELEN        (4 * TCP_SND_BUF / TCP_MSS)
#define LWIP_TCP_KEEPALIVE      1
#define SO_REUSE                1
