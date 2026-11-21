// USB host on the Synopsys DWC2 OTG controller (buffer DMA, polled, no IRQ).
// A kernel thread waits for a device on the root port and enumerates it: a
// hub (one level, rescanned every second), a HID boot keyboard (reports become
// console input, VT100 sequences for arrows etc.) or a CDC-ECM Ethernet
// adapter (frames go to the network stack). Unplugging the root device sends
// the thread back to waiting, so hotplug works.
// Every transfer runs with IRQs masked: the controller and its DMA buffer are
// shared between this thread and network code called from syscalls.
// shortcut: no split transactions, so a low/full-speed device behind a
// high-speed (USB 2.0) hub will not work; polled, US keyboard layout, no LEDs.
#include "kernel.h"
#include "mailbox.h"
#include "mm.h"
#include "mmio.h"
#include "net.h"
#include "sched.h"
#include "timer.h"
#include "usb.h"
#include "vfs.h"

#define USB         (PERIPHERAL_BASE + 0x980000)
#define GAHBCFG     (USB + 0x008)
#define GUSBCFG     (USB + 0x00C)
#define GRSTCTL     (USB + 0x010)
#define GRXFSIZ     (USB + 0x024)
#define GNPTXFSIZ   (USB + 0x028)
#define HPTXFSIZ    (USB + 0x100)
#define HCFG        (USB + 0x400)
#define HFNUM       (USB + 0x408)
#define HPRT        (USB + 0x440)
#define PCGCCTL     (USB + 0xE00)
#define HCCHAR(n)   (USB + 0x500 + (n) * 0x20)
#define HCSPLT(n)   (USB + 0x504 + (n) * 0x20)
#define HCINT(n)    (USB + 0x508 + (n) * 0x20)
#define HCINTMSK(n) (USB + 0x50C + (n) * 0x20)
#define HCTSIZ(n)   (USB + 0x510 + (n) * 0x20)
#define HCDMA(n)    (USB + 0x514 + (n) * 0x20)

#define HPRT_CONN     (1u << 0)
#define HPRT_ENA      (1u << 2)
#define HPRT_RST      (1u << 8)
#define HPRT_PWR      (1u << 12)
#define HPRT_W1C      0x2eu         // write-1-to-clear bits; never write them back
#define HCINT_DONE    (1u << 0)
#define HCINT_HALTED  (1u << 1)
#define HCINT_STALL   (1u << 3)
#define HCINT_NAK     (1u << 4)
#define HCINT_ERRS    ((1u << 2) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10))

#define PID_DATA0 0
#define PID_DATA1 2
#define PID_SETUP 3
#define EP_CONTROL 0
#define EP_INTR    3

#define BUS_UNCACHED 0xC0000000u    // ARM phys -> VC bus address for DMA

#define EP_BULK    2

static uint8_t __attribute__((aligned(64))) dma[2048];

static void cache(void *p, size_t len)
{
    for (uintptr_t a = (uintptr_t)p & ~63ul; a < (uintptr_t)p + len; a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

#define EPI(ep, in) ((ep) | ((in) ? 16 : 0))

// One transfer on channel 0. Returns bytes moved, or USB_ERR/USB_NAK/USB_STALL.
static int xfer(struct usb_dev *d, int ep, int type, int in, int pid, void *buf, int len)
{
    int mps = ep ? d->mps[EPI(ep, in)] : d->mps0;
    if (len > (int)sizeof dma || mps == 0)
        return USB_ERR;
    uint64_t daif = read_sysreg(daif);
    irq_disable();
    if (!in && len)
        memcpy(dma, buf, (size_t)len);
    cache(dma, (size_t)(len ? len : 1));
    mmio_write(HCINT(0), 0xffffffff);
    mmio_write(HCINTMSK(0), 0x7ff);
    mmio_write(HCSPLT(0), 0);
    mmio_write(HCTSIZ(0), (uint32_t)len | (uint32_t)(len ? (len + mps - 1) / mps : 1) << 19 |
                          (uint32_t)pid << 29);
    mmio_write(HCDMA(0), (uint32_t)V2P(dma) | BUS_UNCACHED);
    uint32_t odd = !(mmio_read(HFNUM) & 1);    // periodic: run in the next frame
    mmio_write(HCCHAR(0), (uint32_t)mps | (uint32_t)ep << 11 | (uint32_t)in << 15 |
                          (uint32_t)d->low_speed << 17 | (uint32_t)type << 18 | 1u << 20 |
                          (uint32_t)d->addr << 22 | odd << 29 | 1u << 31);
    uint32_t st;
    int spins = 0, r;
    while (!((st = mmio_read(HCINT(0))) & HCINT_HALTED)) {
        if (++spins > 100000) {             // ~100 ms: stuck channel, disable it
            mmio_write(HCCHAR(0), mmio_read(HCCHAR(0)) | 1u << 30 | 1u << 31);
            write_sysreg(daif, daif);
            return USB_ERR;
        }
        udelay(1);
    }
    if (st & HCINT_STALL)
        r = USB_STALL;
    else if (st & HCINT_ERRS)
        r = USB_ERR;
    else if (st & HCINT_NAK)
        r = USB_NAK;
    else if (!(st & HCINT_DONE))
        r = USB_ERR;
    else
        r = in ? len - (int)(mmio_read(HCTSIZ(0)) & 0x7ffff) : len;
    if (in && r > 0) {
        cache(dma, (size_t)r);
        memcpy(buf, dma, (size_t)r);
    }
    write_sysreg(daif, daif);
    return r;
}

static int retry(struct usb_dev *d, int in, int pid, void *buf, int len)
{
    for (int i = 0; i < 100; i++) {         // control endpoints may NAK while busy
        int r = xfer(d, 0, EP_CONTROL, in, pid, buf, len);
        if (r != USB_NAK)
            return r;
        msleep(1);
    }
    return USB_ERR;
}

int usb_control(struct usb_dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                void *data, uint16_t len)
{
    uint8_t setup[8] = { type, req, (uint8_t)value, (uint8_t)(value >> 8),
                         (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    int in = type & 0x80, n = 0;
    if (retry(d, 0, PID_SETUP, setup, 8) != 8)
        return USB_ERR;
    if (len && (n = retry(d, in != 0, PID_DATA1, data, len)) < 0)
        return n;
    if (retry(d, !(in && len), PID_DATA1, NULL, 0) < 0)     // status stage
        return USB_ERR;
    return n;
}

// Bulk or interrupt transfer with per-endpoint data toggles. A NAK (no data
// yet) returns USB_NAK without flipping the toggle.
static int data_xfer(struct usb_dev *d, int type, int ep, int in, void *buf, int len)
{
    uint8_t *tog = &d->toggle[EPI(ep, in)];
    int r = xfer(d, ep, type, in, *tog ? PID_DATA1 : PID_DATA0, buf, len);
    if (r >= 0) {
        int mps = d->mps[EPI(ep, in)];
        int pkts = r ? (r + mps - 1) / mps : 1;
        if (!in && len)
            pkts = (len + mps - 1) / mps;
        *tog ^= pkts & 1;
    }
    return r;
}

int usb_bulk(struct usb_dev *d, int ep, int in, void *buf, int len)
{
    return data_xfer(d, EP_BULK, ep, in, buf, len);
}

int usb_intr(struct usb_dev *d, int ep, void *buf, int len)
{
    return data_xfer(d, EP_INTR, ep, 1, buf, len);
}

// Non-blocking bulk IN on channel 1. In buffer-DMA mode the core retries NAKs
// on bulk endpoints by itself, so a receive only completes when data arrives:
// keep one armed and check it, instead of blocking (and timing out) on ch 0.
static uint8_t __attribute__((aligned(64))) rxdma[2048];
static struct usb_dev *rx_dev;
static int rx_ep, rx_len;

int usb_bulk_rx(struct usb_dev *d, int ep, void *buf, int len)
{
    if (len > (int)sizeof rxdma)
        return USB_ERR;
    uint64_t daif = read_sysreg(daif);
    irq_disable();
    int r = USB_NAK;
    if (rx_dev != d || rx_ep != ep) {           // arm
        int mps = d->mps[EPI(ep, 1)];
        rx_dev = d, rx_ep = ep, rx_len = len;
        cache(rxdma, (size_t)len);
        mmio_write(HCINT(1), 0xffffffff);
        mmio_write(HCINTMSK(1), 0x7ff);
        mmio_write(HCSPLT(1), 0);
        mmio_write(HCTSIZ(1), (uint32_t)len | (uint32_t)((len + mps - 1) / mps) << 19 |
                              (uint32_t)(d->toggle[EPI(ep, 1)] ? PID_DATA1 : PID_DATA0) << 29);
        mmio_write(HCDMA(1), (uint32_t)V2P(rxdma) | BUS_UNCACHED);
        mmio_write(HCCHAR(1), (uint32_t)mps | (uint32_t)ep << 11 | 1u << 15 |
                              (uint32_t)d->low_speed << 17 | (uint32_t)EP_BULK << 18 | 1u << 20 |
                              (uint32_t)d->addr << 22 | 1u << 31);
    } else {
        uint32_t st = mmio_read(HCINT(1));
        if (st & HCINT_HALTED) {
            rx_dev = NULL;                      // re-armed on the next call
            if (st & HCINT_STALL)
                r = USB_STALL;
            else if (st & HCINT_ERRS)
                r = USB_ERR;
            else if (st & HCINT_DONE) {
                int n = rx_len - (int)(mmio_read(HCTSIZ(1)) & 0x7ffff);
                int mps = d->mps[EPI(ep, 1)];
                d->toggle[EPI(ep, 1)] ^= (n ? (n + mps - 1) / mps : 1) & 1;
                cache(rxdma, (size_t)(n ? n : 1));
                memcpy(buf, rxdma, (size_t)(n < len ? n : len));
                r = n;
            }
        }
    }
    write_sysreg(daif, daif);
    return r;
}

void usb_bulk_rx_cancel(void)
{
    if (rx_dev && (mmio_read(HCCHAR(1)) & 1u << 31)) {
        mmio_write(HCCHAR(1), mmio_read(HCCHAR(1)) | 1u << 30 | 1u << 31);     // disable
        for (int i = 0; i < 1000 && !(mmio_read(HCINT(1)) & HCINT_HALTED); i++)
            udelay(1);
    }
    rx_dev = NULL;
}

static int core_init(void)
{
    if (mbox_power_on(3) != 0)
        return -1;
    int spins = 0;
    while (!(mmio_read(GRSTCTL) & (1u << 31)))     // AHB idle
        if (++spins > 100000)
            return -1;
        else
            udelay(1);
    mmio_write(GRSTCTL, 1);                         // core soft reset
    for (spins = 0; mmio_read(GRSTCTL) & 1; spins++)
        if (spins > 100000)
            return -1;
        else
            udelay(1);
    msleep(100);
    mmio_write(GUSBCFG, (mmio_read(GUSBCFG) & ~(1u << 30)) | 1u << 29);    // force host mode
    msleep(50);
    mmio_write(PCGCCTL, 0);
    mmio_write(HCFG, mmio_read(HCFG) & ~3u);        // PHY clock 30/60 MHz
    mmio_write(GRXFSIZ, 0x200);
    mmio_write(GNPTXFSIZ, 0x200u << 16 | 0x200);
    mmio_write(HPTXFSIZ, 0x200u << 16 | 0x400);
    mmio_write(GRSTCTL, 0x10u << 6 | 1u << 5);      // flush all TX FIFOs
    udelay(100);
    mmio_write(GRSTCTL, 1u << 4);                   // flush RX FIFO
    udelay(100);
    mmio_write(GAHBCFG, 1u << 5);                   // DMA on, global IRQ off (polled)
    mmio_write(HPRT, (mmio_read(HPRT) & ~HPRT_W1C) | HPRT_PWR);
    return 0;
}

// ---- enumeration ----

static uint8_t cfgbuf[512];

// Record every endpoint of the configuration (packet sizes) and reset toggles.
static void note_endpoints(struct usb_dev *d, const uint8_t *c, int total)
{
    memset(d->mps, 0, sizeof d->mps);
    memset(d->toggle, 0, sizeof d->toggle);
    for (int i = 0; i + 2 <= total && c[i] >= 2; i += c[i])
        if (c[i + 1] == 5)
            d->mps[EPI(c[i + 2] & 0xf, c[i + 2] & 0x80)] = (uint16_t)(c[i + 4] | (c[i + 5] & 7) << 8);
}

static int hid_keyboard_probe(struct usb_dev *d, const uint8_t *c, int total);

// Enumerate the device that was just reset (it answers at address 0), give it
// address addr, and hand it to the first driver that recognizes one of its
// configurations.
static enum usb_kind enumerate(struct usb_dev *d, int addr, int low_speed)
{
    uint8_t dd[18];
    *d = (struct usb_dev){ .mps0 = 8, .low_speed = low_speed };
    if (usb_control(d, 0x80, 6, 0x0100, 0, dd, 8) != 8)
        return USB_NONE;
    d->mps0 = dd[7] ? dd[7] : 8;
    if (usb_control(d, 0x00, 5, (uint16_t)addr, 0, NULL, 0) < 0)   // SET_ADDRESS
        return USB_NONE;
    d->addr = addr;
    msleep(10);
    if (usb_control(d, 0x80, 6, 0x0100, 0, dd, 18) != 18)
        return USB_NONE;
    for (int ci = 0; ci < dd[17]; ci++) {
        if (usb_control(d, 0x80, 6, (uint16_t)(0x0200 | ci), 0, cfgbuf, 9) != 9)
            continue;
        int total = cfgbuf[2] | cfgbuf[3] << 8;
        if (total > (int)sizeof cfgbuf)
            total = sizeof cfgbuf;
        if (usb_control(d, 0x80, 6, (uint16_t)(0x0200 | ci), 0, cfgbuf, (uint16_t)total) != total)
            continue;
        note_endpoints(d, cfgbuf, total);
        int hub = 0;
        for (int i = 0; i + 2 <= total && cfgbuf[i] >= 2; i += cfgbuf[i])
            hub |= cfgbuf[i + 1] == 4 && cfgbuf[i + 5] == 9;
        if (hub)
            return usb_control(d, 0x00, 9, cfgbuf[5], 0, NULL, 0) < 0 ? USB_NONE : USB_HUB;
        if (hid_keyboard_probe(d, cfgbuf, total))
            return USB_KEYBOARD;
        if (usb_ecm_probe(d, cfgbuf, total))
            return USB_NET;
    }
    return USB_NONE;
}

// ---- hub (class requests on the hub's default pipe) ----

#define PORT_RESET 4
#define PORT_POWER 8
#define C_PORT_CONNECTION 16
#define C_PORT_RESET 20

static int hub_ports(struct usb_dev *hub)
{
    uint8_t hd[16];
    if (usb_control(hub, 0xa0, 6, 0x2900, 0, hd, sizeof hd) < 3)       // hub descriptor
        return 0;
    int n = hd[2];
    for (int p = 1; p <= n; p++)
        usb_control(hub, 0x23, 3, PORT_POWER, (uint16_t)p, NULL, 0);   // SET_FEATURE
    msleep(hd[5] * 2 + 100);                                        // power-on to good
    return n;
}

static uint32_t port_status(struct usb_dev *hub, int p)
{
    uint8_t st[4] = { 0 };
    if (usb_control(hub, 0xa3, 0, 0, (uint16_t)p, st, 4) != 4)
        return 0;
    return st[0] | st[1] << 8 | (uint32_t)st[2] << 16 | (uint32_t)st[3] << 24;
}

// Reset hub port p if a device was plugged in since the last look; returns 0
// (full speed), 1 (low speed), -1 (nothing new), -2 (high speed).
static int hub_port_reset(struct usb_dev *hub, int p)
{
    uint32_t now = port_status(hub, p);
    if (!(now & 1) || !(now & (1u << 16)))     // connected and connection changed
        return -1;
    usb_control(hub, 0x23, 3, PORT_RESET, (uint16_t)p, NULL, 0);
    uint32_t st = 0;
    for (int i = 0; i < 20; i++) {
        msleep(20);
        st = port_status(hub, p);
        if (!(st & (1u << 4)))                  // reset finished
            break;
    }
    usb_control(hub, 0x01 | 0x22, 1, C_PORT_RESET, (uint16_t)p, NULL, 0);      // CLEAR_FEATURE
    usb_control(hub, 0x01 | 0x22, 1, C_PORT_CONNECTION, (uint16_t)p, NULL, 0);
    msleep(20);
    if (!(st & 2))
        return -1;                              // not enabled
    return st & (1u << 10) ? -2 : (st >> 9) & 1;
}

// ---- HID boot reports -> console bytes ----

static const char plain[] = "abcdefghijklmnopqrstuvwxyz1234567890\r\033\x7f\t -=[]\\#;'`,./";
static const char shifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\033\x7f\t _+{}|~:\"~<>?";

static void emit(const char *s)
{
    uint64_t daif = read_sysreg(daif);
    irq_disable();                  // console_input normally runs in the UART IRQ
    for (; *s; s++)
        console_input(*s);
    write_sysreg(daif, daif);
}

static int caps;

static void press(uint8_t mod, uint8_t key)
{
    static const struct { uint8_t key; const char *seq; } special[] = {
        { 0x4f, "\033[C" }, { 0x50, "\033[D" }, { 0x51, "\033[B" }, { 0x52, "\033[A" },
        { 0x4a, "\033[H" }, { 0x4d, "\033[F" }, { 0x4c, "\033[3~" }, { 0x4b, "\033[5~" },
        { 0x4e, "\033[6~" },
    };
    for (unsigned i = 0; i < sizeof special / sizeof *special; i++)
        if (special[i].key == key) {
            emit(special[i].seq);
            return;
        }
    if (key == 0x39) {
        caps ^= 1;
        return;
    }
    if (key < 0x04 || key > 0x38)
        return;
    int shift = (mod & 0x22) != 0, ctrl = (mod & 0x11) != 0;
    char c = (shift ? shifted : plain)[key - 4];
    if (caps && c >= 'a' && c <= 'z')
        c -= 32;
    else if (caps && c >= 'A' && c <= 'Z')
        c += 32;
    if (ctrl && ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'))
        c = (char)((c | 0x20) & 0x1f);
    char s[2] = { c, 0 };
    if (c)
        emit(s);
}

static void keyboard_poll(struct usb_dev *d)
{
    uint8_t rep[8];
    uint64_t now = timer_ticks();
    int n = usb_intr(d, d->kbd_ep, rep, d->mps[EPI(d->kbd_ep, 1)] < 8 ? d->mps[EPI(d->kbd_ep, 1)] : 8);
    if (n == USB_NAK || n == 0) {
        if (d->held && now - d->held_since >= 50 && now - d->last_repeat >= 4) {  // 500 ms, then 25/s
            press(d->held_mod, d->held);
            d->last_repeat = now;
        }
        return;
    }
    if (n < 0) {
        kprintf("[USB] keyboard gone\n");
        d->kind = USB_NONE;
        return;
    }
    if (n < 3)
        return;
    d->held = 0;
    for (int i = 2; i < n; i++) {
        if (rep[i] < 4)
            continue;           // none / rollover error
        int seen = 0;
        for (int j = 2; j < 8; j++)
            seen |= d->prev[j] == rep[i];
        if (!seen)
            press(rep[0], rep[i]);
        d->held = rep[i], d->held_mod = rep[0], d->held_since = d->last_repeat = now;
    }
    memset(d->prev, 0, sizeof d->prev);
    memcpy(d->prev, rep, (size_t)n);
}

static int hid_keyboard_probe(struct usb_dev *d, const uint8_t *c, int total)
{
    int iface = -1, cur = -1, is_kbd = 0;
    for (int i = 0; i + 2 <= total && c[i] >= 2; i += c[i]) {
        if (c[i + 1] == 4) {
            cur = c[i + 2];
            is_kbd = c[i + 5] == 3 && c[i + 6] == 1 && c[i + 7] == 1;
        } else if (c[i + 1] == 5 && is_kbd && iface < 0 && (c[i + 2] & 0x80) && (c[i + 3] & 3) == 3) {
            iface = cur;
            d->kbd_ep = c[i + 2] & 0xf;
        }
    }
    if (iface < 0 || usb_control(d, 0x00, 9, c[5], 0, NULL, 0) < 0)     // SET_CONFIGURATION
        return 0;
    usb_control(d, 0x21, 0x0b, 0, (uint16_t)iface, NULL, 0);           // SET_PROTOCOL boot
    usb_control(d, 0x21, 0x0a, 0, (uint16_t)iface, NULL, 0);           // SET_IDLE: report on change
    kprintf("[USB] keyboard ready (address %d, %s speed)\n", d->addr, d->low_speed ? "low" : "full");
    return 1;
}

// ---- thread ----

#define MAX_DEVS 8
static struct usb_dev devs[MAX_DEVS];

static int root_reset(void)    // 1 low speed, 0 otherwise, -1 failed
{
    uint32_t p = mmio_read(HPRT) & ~HPRT_W1C;
    mmio_write(HPRT, p | HPRT_RST);
    msleep(60);
    mmio_write(HPRT, p & ~HPRT_RST);
    msleep(20);
    p = mmio_read(HPRT);
    if (!(p & HPRT_ENA))
        return -1;
    return ((p >> 17) & 3) == 2;
}

static void add_device(int port_speed, int *next_addr)
{
    for (int i = 1; i < MAX_DEVS; i++)
        if (devs[i].kind == USB_NONE) {
            if (*next_addr > 127)
                *next_addr = 2;
            devs[i].kind = enumerate(&devs[i], (*next_addr)++, port_speed);
            return;
        }
}

static void usb_thread(void *arg)
{
    (void)arg;
    if (core_init() != 0) {
        kprintf("[USB] controller init failed\n");
        return;
    }
    kprintf("[USB] host controller up\n");
    struct usb_dev *root = &devs[0];
    for (;;) {
        while (!(mmio_read(HPRT) & HPRT_CONN))
            msleep(200);
        msleep(100);                // debounce
        mmio_write(HPRT, (mmio_read(HPRT) & ~HPRT_W1C) | 0x2);     // clear connect-detected
        int ls = root_reset();
        root->kind = ls < 0 ? USB_NONE : enumerate(root, 1, ls);
        if (root->kind == USB_NONE) {
            kprintf("[USB] device on root port: no driver\n");
        } else if (root->kind == USB_HUB && !(mmio_read(HPRT) & (3u << 17))) {
            kprintf("[USB] high-speed hub: not supported yet (no split transactions)\n");
            root->kind = USB_NONE;
        }
        int nports = root->kind == USB_HUB ? hub_ports(root) : 0, next = 2;
        if (nports)
            kprintf("[USB] hub with %d ports\n", nports);
        uint64_t last_scan = 0;
        int busy = 0;               // polls left at full speed after network traffic
        while (mmio_read(HPRT) & HPRT_CONN) {
            if (nports && timer_ticks() - last_scan >= HZ) {
                last_scan = timer_ticks();
                for (int p = 1; p <= nports; p++) {
                    int sp = hub_port_reset(root, p);
                    if (sp >= 0)
                        add_device(sp, &next);
                }
            }
            for (int i = 0; i < MAX_DEVS; i++) {
                if (devs[i].kind == USB_KEYBOARD)
                    keyboard_poll(&devs[i]);
                else if (devs[i].kind == USB_NET && usb_ecm_poll(&devs[i]))
                    busy = 2000;
            }
            // While frames flow, poll back-to-back (yielding the CPU); idle: every tick.
            if (busy && busy--)
                schedule();
            else
                msleep(10);
        }
        usb_bulk_rx_cancel();
        for (int i = 0; i < MAX_DEVS; i++) {
            if (devs[i].kind == USB_NET)
                usb_ecm_gone(&devs[i]);
            devs[i].kind = USB_NONE;
        }
        kprintf("[USB] root device unplugged\n");
    }
}

void usb_init(void)
{
    if (kthread_create(usb_thread, NULL, "usb") < 0)
        kprintf("[USB] cannot start thread\n");
}
