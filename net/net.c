// lwIP glue: one Ethernet interface on the USB adapter, configured by DHCP.
#include "kernel.h"
#include "sched.h"
#include "net.h"
#include "timer.h"
#include "vfs.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"

static struct netif nif;
static int attached;
static int (*tx)(const void *frame, int len);
static uint8_t txbuf[1600];

uint32_t sys_now(void) { return (uint32_t)(timer_ticks() * (1000 / HZ)); }

unsigned net_rand(void)
{
    static uint64_t x;
    x = x * 6364136223846793005ul + read_sysreg(cntpct_el0);
    return (unsigned)(x >> 33);
}

static err_t linkoutput(struct netif *n, struct pbuf *p)
{
    (void)n;
    if (!tx || p->tot_len > sizeof txbuf)
        return ERR_IF;
    pbuf_copy_partial(p, txbuf, p->tot_len, 0);
    return tx(txbuf, p->tot_len) == 0 ? ERR_OK : ERR_IF;
}

static err_t nif_init(struct netif *n)
{
    n->name[0] = 'e', n->name[1] = 'n';
    n->output = etharp_output;
    n->linkoutput = linkoutput;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
    netif_set_hostname(n, "pios");
    return ERR_OK;
}

// Write /etc/resolv.conf so programs (musl's resolver) find the DNS server.
static void write_resolv(void)
{
    const ip_addr_t *dns = dns_getserver(0);
    if (ip_addr_isany(dns))
        return;
    char line[48], ip[16];
    ip4addr_ntoa_r(dns, ip, sizeof ip);
    size_t n = 0;
    for (const char *s = "nameserver "; *s; s++)
        line[n++] = *s;
    for (const char *s = ip; *s; s++)
        line[n++] = *s;
    line[n++] = '\n';
    struct inode *ino;
    if (vfs_lookup("/etc/resolv.conf", &ino) < 0 && vfs_create("/etc/resolv.conf", S_IFREG | 0644, &ino) < 0)
        return;
    ino->ops->truncate(ino);
    ino->ops->write(ino, line, n, 0);
    iput(ino);
}

static void status_changed(struct netif *n)
{
    if (ip4_addr_isany_val(*netif_ip4_addr(n)))
        return;
    char ip[16], mask[16], gw[16];
    ip4addr_ntoa_r(netif_ip4_addr(n), ip, sizeof ip);
    ip4addr_ntoa_r(netif_ip4_netmask(n), mask, sizeof mask);
    ip4addr_ntoa_r(netif_ip4_gw(n), gw, sizeof gw);
    kprintf("[NET] en0 %s netmask %s gateway %s\n", ip, mask, gw);
    write_resolv();
}

// Timers and loopback delivery, every tick, whether or not an adapter exists.
static void net_thread(void *arg)
{
    (void)arg;
    for (;;) {
        net_poll();
        msleep(10);
    }
}

void net_init(void)
{
    lwip_init();
    if (kthread_create(net_thread, NULL, "net") < 0)
        kprintf("[NET] cannot start thread\n");
}

void net_attach(const uint8_t mac[6], int (*send)(const void *frame, int len))
{
    NET_LOCK();
    tx = send;
    if (!attached) {
        netif_add(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, nif_init, ethernet_input);
        netif_set_status_callback(&nif, status_changed);
        netif_set_default(&nif);
        attached = 1;
    }
    memcpy(nif.hwaddr, mac, 6);
    netif_set_up(&nif);
    netif_set_link_up(&nif);
    dhcp_start(&nif);
    NET_UNLOCK();
    kprintf("[NET] en0 up, asking DHCP\n");
}

void net_detach(void)
{
    NET_LOCK();
    tx = NULL;
    if (attached) {
        dhcp_stop(&nif);
        netif_set_link_down(&nif);
        netif_set_down(&nif);
    }
    NET_UNLOCK();
    kprintf("[NET] en0 down\n");
}

void net_input(const void *frame, int len)
{
    NET_LOCK();
    struct pbuf *p = attached ? pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL) : NULL;
    if (p) {
        pbuf_take(p, frame, (u16_t)len);
        if (nif.input(p, &nif) != ERR_OK)
            pbuf_free(p);
    }
    NET_UNLOCK();
}

void net_poll(void)
{
    NET_LOCK();
    sys_check_timeouts();
    netif_poll_all();
    NET_UNLOCK();
}
