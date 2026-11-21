// VideoCore mailbox 0, property channel 8. The buffer is cleaned/invalidated
// around each call, so it works before and after caches are on.
#include "kernel.h"
#include "mailbox.h"
#include "mmio.h"

#define MBOX_BASE   (PERIPHERAL_BASE + 0xB880)
#define MBOX_READ   (MBOX_BASE + 0x00)
#define MBOX_STATUS (MBOX_BASE + 0x18)
#define MBOX_WRITE  (MBOX_BASE + 0x20)
#define MBOX_FULL   0x80000000u
#define MBOX_EMPTY  0x40000000u
#define CH_PROP     8
#define BUS_UNCACHED 0xC0000000u    // ARM phys -> VC bus address alias (BCM2837)

static volatile uint32_t __attribute__((aligned(64))) buf[8];   // one cache line

static void dcache_civac_buf(void)
{
    __asm__ volatile("dc civac, %0\n dsb sy" :: "r"(buf) : "memory");
}

static int mbox_call(void)
{
    uint32_t msg = ((uint32_t)(uintptr_t)buf | BUS_UNCACHED) | CH_PROP;
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
    buf[0] = sizeof buf;
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
    buf[0] = sizeof buf;
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
