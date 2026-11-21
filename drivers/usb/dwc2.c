// USB keyboard on the Synopsys DWC2 OTG controller (host mode, buffer DMA).
// A kernel thread waits for a device on the root port, enumerates it, and if
// it is a HID boot keyboard polls its interrupt endpoint every 10 ms, turning
// reports into console input (VT100 sequences for arrows etc.). Unplugging
// sends it back to waiting, so hotplug works.
// One hub level is supported (QEMU's Pi model has a hub on the root port, so do
// many Pi Zero setups); hub ports are rescanned every second while no keyboard
// is active.
// shortcut: no split transactions, so a low/full-speed keyboard behind a
// high-speed (USB 2.0) hub will not enumerate; polled (no IRQ), US layout, no
// LEDs, one keyboard. Add HCSPLT handling when USB 2.0 hubs matter.
#include "kernel.h"
#include "mailbox.h"
#include "mm.h"
#include "mmio.h"
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

enum { OK = 0, ERR = -1, NAK = -2, STALL = -3 };

struct dev {
    int addr, low_speed, mps0;
    int ep, ep_mps, toggle;         // keyboard interrupt IN endpoint
};

static uint8_t __attribute__((aligned(64))) dma[512];

static void cache(void *p, size_t len, int invalidate)
{
    for (uintptr_t a = (uintptr_t)p & ~63ul; a < (uintptr_t)p + len; a += 64)
        if (invalidate)
            __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else
            __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// One transfer on channel ch. Returns bytes moved, or ERR/NAK/STALL.
static int xfer(int ch, struct dev *d, int ep, int type, int in, int pid, void *buf, int len)
{
    int mps = ep ? d->ep_mps : d->mps0;
    if (len > (int)sizeof dma)
        return ERR;
    if (!in && len)
        memcpy(dma, buf, (size_t)len);
    cache(dma, sizeof dma, 1);
    mmio_write(HCINT(ch), 0xffffffff);
    mmio_write(HCINTMSK(ch), 0x7ff);
    mmio_write(HCSPLT(ch), 0);
    mmio_write(HCTSIZ(ch), (uint32_t)len | (uint32_t)(len ? (len + mps - 1) / mps : 1) << 19 |
                           (uint32_t)pid << 29);
    mmio_write(HCDMA(ch), (uint32_t)V2P(dma) | BUS_UNCACHED);
    uint32_t odd = !(mmio_read(HFNUM) & 1);    // periodic: run in the next frame
    mmio_write(HCCHAR(ch), (uint32_t)mps | (uint32_t)ep << 11 | (uint32_t)in << 15 |
                           (uint32_t)d->low_speed << 17 | (uint32_t)type << 18 | 1u << 20 |
                           (uint32_t)d->addr << 22 | odd << 29 | 1u << 31);
    uint32_t st;
    int spins = 0;
    while (!((st = mmio_read(HCINT(ch))) & HCINT_HALTED)) {
        if (++spins > 100000) {             // ~100 ms: stuck channel, disable it
            mmio_write(HCCHAR(ch), mmio_read(HCCHAR(ch)) | 1u << 30 | 1u << 31);
            return ERR;
        }
        udelay(1);
    }
    if (st & HCINT_STALL)
        return STALL;
    if (st & HCINT_ERRS)
        return ERR;
    if (st & HCINT_NAK)
        return NAK;
    if (!(st & HCINT_DONE))
        return ERR;
    int n = in ? len - (int)(mmio_read(HCTSIZ(ch)) & 0x7ffff) : len;
    if (in && n > 0) {
        cache(dma, (size_t)n, 1);
        memcpy(buf, dma, (size_t)n);
    }
    return n;
}

static int retry(int ch, struct dev *d, int in, int pid, void *buf, int len)
{
    for (int i = 0; i < 100; i++) {         // control endpoints may NAK while busy
        int r = xfer(ch, d, 0, EP_CONTROL, in, pid, buf, len);
        if (r != NAK)
            return r;
        msleep(1);
    }
    return ERR;
}

static int control(struct dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                   void *data, uint16_t len)
{
    uint8_t setup[8] = { type, req, (uint8_t)value, (uint8_t)(value >> 8),
                         (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    int in = type & 0x80, n = 0;
    if (retry(0, d, 0, PID_SETUP, setup, 8) != 8)
        return ERR;
    if (len && (n = retry(0, d, in != 0, PID_DATA1, data, len)) < 0)
        return n;
    if (retry(0, d, !(in && len), PID_DATA1, NULL, 0) < 0)     // status stage
        return ERR;
    return n;
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

enum kind { NONE, KEYBOARD, HUB };

// Enumerate the device that was just reset (it answers at address 0) and give
// it address addr. Configures it if it is a boot keyboard or a hub.
static enum kind enumerate(struct dev *d, int addr, int low_speed)
{
    uint8_t desc[256];
    *d = (struct dev){ .mps0 = 8, .low_speed = low_speed };
    if (control(d, 0x80, 6, 0x0100, 0, desc, 8) != 8)
        return NONE;
    d->mps0 = desc[7] ? desc[7] : 8;
    if (control(d, 0x00, 5, (uint16_t)addr, 0, NULL, 0) < 0)   // SET_ADDRESS
        return NONE;
    d->addr = addr;
    msleep(10);
    if (control(d, 0x80, 6, 0x0200, 0, desc, 9) != 9)          // config descriptor header
        return NONE;
    int total = desc[2] | desc[3] << 8;
    if (total > (int)sizeof desc)
        total = sizeof desc;
    if (control(d, 0x80, 6, 0x0200, 0, desc, (uint16_t)total) != total)
        return NONE;
    int config = desc[5], iface = -1, cur = -1, is_kbd = 0, is_hub = 0;
    for (int i = 0; i + 2 <= total && desc[i] >= 2; i += desc[i]) {
        if (desc[i + 1] == 4) {                     // interface
            cur = desc[i + 2];
            is_kbd = desc[i + 5] == 3 && desc[i + 6] == 1 && desc[i + 7] == 1;
            is_hub |= desc[i + 5] == 9;
        } else if (desc[i + 1] == 5 && is_kbd && iface < 0 &&   // endpoint
                   (desc[i + 2] & 0x80) && (desc[i + 3] & 3) == 3) {
            iface = cur;
            d->ep = desc[i + 2] & 0xf;
            d->ep_mps = desc[i + 4] | (desc[i + 5] & 7) << 8;
        }
    }
    if (iface < 0 && !is_hub)
        return NONE;
    if (control(d, 0x00, 9, (uint16_t)config, 0, NULL, 0) < 0)     // SET_CONFIGURATION
        return NONE;
    if (is_hub)
        return HUB;
    control(d, 0x21, 0x0b, 0, (uint16_t)iface, NULL, 0);           // SET_PROTOCOL boot
    control(d, 0x21, 0x0a, 0, (uint16_t)iface, NULL, 0);           // SET_IDLE: report on change
    return KEYBOARD;
}

// ---- hub (class requests on the hub's default pipe) ----

#define PORT_RESET 4
#define PORT_POWER 8
#define C_PORT_CONNECTION 16
#define C_PORT_RESET 20

static int hub_ports(struct dev *hub)
{
    uint8_t hd[16];
    if (control(hub, 0xa0, 6, 0x2900, 0, hd, sizeof hd) < 3)       // hub descriptor
        return 0;
    int n = hd[2];
    for (int p = 1; p <= n; p++)
        control(hub, 0x23, 3, PORT_POWER, (uint16_t)p, NULL, 0);   // SET_FEATURE
    msleep(hd[5] * 2 + 100);                                        // power-on to good
    return n;
}

static uint32_t port_status(struct dev *hub, int p)
{
    uint8_t st[4] = { 0 };
    if (control(hub, 0xa3, 0, 0, (uint16_t)p, st, 4) != 4)
        return 0;
    return st[0] | st[1] << 8 | (uint32_t)st[2] << 16 | (uint32_t)st[3] << 24;
}

// Reset hub port p if a device was plugged in since the last look; returns 0
// (full speed), 1 (low speed), -1 (nothing new), -2 (high speed).
static int hub_port_reset(struct dev *hub, int p)
{
    uint32_t now = port_status(hub, p);
    if (!(now & 1) || !(now & (1u << 16)))     // connected and connection changed
        return -1;
    control(hub, 0x23, 3, PORT_RESET, (uint16_t)p, NULL, 0);
    uint32_t st = 0;
    for (int i = 0; i < 20; i++) {
        msleep(20);
        st = port_status(hub, p);
        if (!(st & (1u << 4)))                  // reset finished
            break;
    }
    control(hub, 0x01 | 0x22, 1, C_PORT_RESET, (uint16_t)p, NULL, 0);      // CLEAR_FEATURE
    control(hub, 0x01 | 0x22, 1, C_PORT_CONNECTION, (uint16_t)p, NULL, 0);
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

static void keyboard_loop(struct dev *d)
{
    uint8_t rep[8], prev[8] = { 0 };
    uint8_t held = 0, held_mod = 0;
    uint64_t held_since = 0, last_repeat = 0;
    d->toggle = PID_DATA0;
    for (;;) {
        msleep(10);
        int n = xfer(1, d, d->ep, EP_INTR, 1, d->toggle, rep, d->ep_mps < 8 ? d->ep_mps : 8);
        uint64_t now = timer_ticks();
        if (n == NAK || n == 0) {
            if (held && now - held_since >= 50 && now - last_repeat >= 4) {  // 500 ms, then 25/s
                press(held_mod, held);
                last_repeat = now;
            }
            continue;
        }
        if (n < 0) {
            kprintf("[USB] keyboard gone\n");
            return;
        }
        d->toggle ^= PID_DATA1;     // DATA0 <-> DATA1 after each good packet
        if (n < 3)
            continue;
        held = 0;
        for (int i = 2; i < n; i++) {
            if (rep[i] < 4)
                continue;           // none / rollover error
            int seen = 0;
            for (int j = 2; j < 8; j++)
                seen |= prev[j] == rep[i];
            if (!seen)
                press(rep[0], rep[i]);
            held = rep[i], held_mod = rep[0], held_since = last_repeat = now;
        }
        memset(prev, 0, sizeof prev);
        memcpy(prev, rep, (size_t)n);
    }
}

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

static void run_keyboard(struct dev *d)
{
    kprintf("[USB] keyboard ready (address %d, %s speed)\n", d->addr, d->low_speed ? "low" : "full");
    keyboard_loop(d);
}

static void usb_thread(void *arg)
{
    (void)arg;
    if (core_init() != 0) {
        kprintf("[USB] controller init failed\n");
        return;
    }
    kprintf("[USB] host controller up, waiting for a keyboard\n");
    struct dev root, d;
    for (;;) {
        while (!(mmio_read(HPRT) & HPRT_CONN))
            msleep(200);
        msleep(100);                // debounce
        mmio_write(HPRT, (mmio_read(HPRT) & ~HPRT_W1C) | 0x2);     // clear connect-detected
        int ls = root_reset();
        enum kind k = ls < 0 ? NONE : enumerate(&root, 1, ls);
        if (k == KEYBOARD) {
            run_keyboard(&root);
            continue;
        }
        if (k == HUB) {
            if (!(mmio_read(HPRT) & (3u << 17))) {
                kprintf("[USB] high-speed hub: not supported yet (no split transactions)\n");
            } else {
                int n = hub_ports(&root), next = 2;
                kprintf("[USB] hub with %d ports\n", n);
                while (mmio_read(HPRT) & HPRT_CONN) {
                    for (int p = 1; p <= n; p++) {
                        int sp = hub_port_reset(&root, p);
                        if (sp < 0)
                            continue;
                        if (next > 127)
                            next = 2;
                        if (enumerate(&d, next++, sp) == KEYBOARD)
                            run_keyboard(&d);
                    }
                    msleep(1000);
                }
                continue;
            }
        } else {
            kprintf("[USB] device on root port is not a keyboard or hub\n");
        }
        while (mmio_read(HPRT) & HPRT_CONN)    // wait for unplug
            msleep(200);
    }
}

void usb_init(void)
{
    if (kthread_create(usb_thread, NULL, "usb") < 0)
        kprintf("[USB] cannot start thread\n");
}
