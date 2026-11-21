// VideoCore mailbox 0, property channel 8. The buffer is cleaned/invalidated
// around each call, so it works before and after caches are on.
#include "kernel.h"
#include "mailbox.h"
#include "mm.h"
#include "mmio.h"

#define MBOX_BASE   (PERIPHERAL_BASE + 0xB880)
#define MBOX_READ   (MBOX_BASE + 0x00)
#define MBOX_STATUS (MBOX_BASE + 0x18)
#define MBOX_WRITE  (MBOX_BASE + 0x20)
#define MBOX_FULL   0x80000000u
#define MBOX_EMPTY  0x40000000u
#define CH_PROP     8
#define BUS_UNCACHED 0xC0000000u    // ARM phys -> VC bus address alias (BCM2837)

static volatile uint32_t __attribute__((aligned(64))) buf[32];  // 128 bytes: two cache lines

static void dcache_civac_buf(void)
{
    for (uintptr_t p = (uintptr_t)buf; p < (uintptr_t)buf + sizeof buf; p += 64)
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static int mbox_call(void)
{
    uint32_t msg = ((uint32_t)V2P(buf) | BUS_UNCACHED) | CH_PROP;
    dcache_civac_buf();
    while (mmio_read(MBOX_STATUS) & MBOX_FULL)
        ;
    mmio_write(MBOX_WRITE, msg);
    for (;;) {
        while (mmio_read(MBOX_STATUS) & MBOX_EMPTY)
            ;
        if (mmio_read(MBOX_READ) == msg) {
            dcache_civac_buf();
            return buf[1] == 0x80000000u ? 0 : -1;  // response code: success
        }
    }
}

int mbox_arm_memory(uint32_t *base, uint32_t *size)
{
    buf[0] = 8 * 4;
    buf[1] = 0;             // request
    buf[2] = 0x00010005;    // tag: get ARM memory
    buf[3] = 8;             // value buffer size
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = 0;             // end tag
    if (mbox_call() != 0 || !(buf[4] & 0x80000000u))
        return -1;
    *base = buf[5];
    *size = buf[6];
    return 0;
}

// Clock rate in Hz for a firmware clock id (1 = EMMC), or 0 on failure.
uint32_t mbox_clock_rate(uint32_t id)
{
    buf[0] = 8 * 4;
    buf[1] = 0;
    buf[2] = 0x00030002;    // tag: get clock rate
    buf[3] = 8;
    buf[4] = 0;
    buf[5] = id;
    buf[6] = 0;
    buf[7] = 0;
    if (mbox_call() != 0 || !(buf[4] & 0x80000000u))
        return 0;
    return buf[6];
}

// Allocate a w x h, 32 bpp framebuffer. Returns 0 and its ARM physical address,
// size and pitch (bytes per row); the firmware may pick a different pitch.
int mbox_framebuffer(uint32_t w, uint32_t h, uint32_t *pa, uint32_t *size, uint32_t *pitch)
{
    static const uint32_t req[] = {
        0, 0,
        0x00048003, 8, 0, 0, 0,     // set physical size (filled below)
        0x00048004, 8, 0, 0, 0,     // set virtual size
        0x00048005, 4, 0, 32,       // set depth
        0x00048006, 4, 0, 0,        // set pixel order: BGR in memory = 0x00RRGGBB words
        0x00040001, 8, 0, 4096, 0,  // allocate buffer, 4 KiB aligned
        0x00040008, 4, 0, 0,        // get pitch
        0,
    };
    for (unsigned i = 0; i < sizeof req / 4; i++)
        buf[i] = req[i];
    buf[0] = sizeof req;
    buf[5] = buf[10] = w;
    buf[6] = buf[11] = h;
    if (mbox_call() != 0 || buf[15] != 32 || !buf[23] || buf[5] != w || buf[6] != h)
        return -1;
    *pa = buf[23] & 0x3fffffff;     // bus address -> ARM physical
    *size = buf[24];
    *pitch = buf[28];
    return 0;
}

int mbox_power_on(uint32_t id)
{
    buf[0] = 8 * 4;
    buf[1] = 0;
    buf[2] = 0x00028001;    // tag: set power state
    buf[3] = 8;
    buf[4] = 0;
    buf[5] = id;
    buf[6] = 3;             // on, wait
    buf[7] = 0;
    if (mbox_call() != 0 || !(buf[4] & 0x80000000u) || !(buf[6] & 1))
        return -1;
    return 0;
}
