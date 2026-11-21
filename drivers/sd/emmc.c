// SD card on the Arasan EMMC controller (SDHCI v3), polled PIO, 512-byte blocks.
// The firmware routes the SD slot to SDHOST; GPIO 48-53 ALT3 reroutes it here
// (takes EMMC away from the Wi-Fi chip, which PiOS does not use).
// shortcut: 1-bit bus at 25 MHz, single-block transfers, no DMA/IRQ; add ACMD6 4-bit + CMD18/25 if I/O speed matters.
#include "kernel.h"
#include "mailbox.h"
#include "mmio.h"
#include "sd.h"

#define GPIO_BASE   (PERIPHERAL_BASE + 0x200000)
#define GPFSEL4     (GPIO_BASE + 0x10)
#define GPFSEL5     (GPIO_BASE + 0x14)
#define GPPUD       (GPIO_BASE + 0x94)
#define GPPUDCLK1   (GPIO_BASE + 0x9C)

#define EMMC        (PERIPHERAL_BASE + 0x300000)
#define BLKSIZECNT  (EMMC + 0x04)
#define ARG1        (EMMC + 0x08)
#define CMDTM       (EMMC + 0x0C)
#define RESP0       (EMMC + 0x10)
#define DATA        (EMMC + 0x20)
#define STATUS      (EMMC + 0x24)
#define CONTROL0    (EMMC + 0x28)
#define CONTROL1    (EMMC + 0x2C)
#define INTERRUPT   (EMMC + 0x30)
#define INT_MASK    (EMMC + 0x34)
#define INT_EN      (EMMC + 0x38)

#define C1_CLK_INTLEN  (1u << 0)
#define C1_CLK_STABLE  (1u << 1)
#define C1_CLK_EN      (1u << 2)
#define C1_SRST_HC     (1u << 24)

#define INT_CMD_DONE   (1u << 0)
#define INT_DATA_DONE  (1u << 1)
#define INT_WRITE_RDY  (1u << 4)
#define INT_READ_RDY   (1u << 5)
#define INT_ERR        0xffff8000u

#define ST_CMD_INHIBIT (1u << 0)
#define ST_DAT_INHIBIT (1u << 1)

// CMDTM: index << 24 | response type | CRC/index check | data | read direction
#define R_NONE  0x00000000u
#define R_136   0x00090000u     // R2: 136-bit, CRC
#define R_48    0x001a0000u     // R1/R6/R7: 48-bit, CRC + index check
#define R_48B   0x001b0000u     // R1b: 48-bit busy
#define R_48NC  0x00020000u     // R3: 48-bit, no checks (OCR)
#define T_DATA  0x00200000u
#define T_READ  0x00000010u
#define CMD(n, flags) (((uint32_t)(n) << 24) | (flags))

static uint32_t rca;
static int block_addressing;    // SDHC/SDXC: argument is LBA, not byte offset
static int ready;

static uint64_t now_us(void)
{
    return read_sysreg(cntpct_el0) * 1000000 / read_sysreg(cntfrq_el0);
}

static void wait_us(uint64_t us)
{
    uint64_t end = now_us() + us;
    while (now_us() < end)
        ;
}

// Wait until (reg & mask) == want; 0 on success, -1 on timeout.
static int wait_reg(uintptr_t reg, uint32_t mask, uint32_t want, uint64_t us)
{
    uint64_t end = now_us() + us;
    while ((mmio_read(reg) & mask) != want)
        if (now_us() > end)
            return -1;
    return 0;
}

// Wait for an interrupt flag; clears it. Returns -1 on error or timeout.
static int wait_int(uint32_t flag, uint64_t us)
{
    uint64_t end = now_us() + us;
    uint32_t v;
    while (!((v = mmio_read(INTERRUPT)) & (flag | INT_ERR)))
        if (now_us() > end)
            return -1;
    mmio_write(INTERRUPT, v & (flag | INT_ERR));
    return (v & INT_ERR) ? -1 : 0;
}

static int cmd(uint32_t cmdtm, uint32_t arg)
{
    if (wait_reg(STATUS, ST_CMD_INHIBIT, 0, 100000) < 0)
        return -1;
    mmio_write(INTERRUPT, 0xffffffff);
    mmio_write(ARG1, arg);
    mmio_write(CMDTM, cmdtm);
    return wait_int(INT_CMD_DONE, 500000);
}

static int acmd(uint32_t cmdtm, uint32_t arg)
{
    return cmd(CMD(55, R_48), rca << 16) < 0 ? -1 : cmd(cmdtm, arg);
}

static int set_clock(uint32_t base, uint32_t hz)
{
    if (wait_reg(STATUS, ST_CMD_INHIBIT | ST_DAT_INHIBIT, 0, 100000) < 0)
        return -1;
    uint32_t div = (base + 2 * hz - 1) / (2 * hz);      // SDHCI v3: f = base / (2 * div)
    if (div > 0x3ff)
        div = 0x3ff;
    uint32_t c1 = mmio_read(CONTROL1) & ~(C1_CLK_EN | 0xffc0u);
    mmio_write(CONTROL1, c1);
    wait_us(10);
    c1 |= ((div & 0xff) << 8) | ((div >> 8) << 6);
    mmio_write(CONTROL1, c1);
    if (wait_reg(CONTROL1, C1_CLK_STABLE, C1_CLK_STABLE, 100000) < 0)
        return -1;
    mmio_write(CONTROL1, c1 | C1_CLK_EN);
    wait_us(10);
    return 0;
}

static void gpio_setup(void)
{
    // GPIO 48, 49 in GPFSEL4 (bits 24-29); 50-53 in GPFSEL5 (bits 0-11). ALT3 = 0b111.
    mmio_write(GPFSEL4, mmio_read(GPFSEL4) | (07u << 24) | (07u << 27));
    mmio_write(GPFSEL5, mmio_read(GPFSEL5) | 07777u);
    mmio_write(GPPUD, 2);                   // pull-up on CMD/DAT lines
    delay_cycles(150);
    mmio_write(GPPUDCLK1, 0x1fu << 17);     // GPIO 49-53
    delay_cycles(150);
    mmio_write(GPPUD, 0);
    mmio_write(GPPUDCLK1, 0);
}

int sd_init(void)
{
    uint32_t base = mbox_clock_rate(1);
    if (!base) {
        kprintf("[SD] cannot read EMMC clock from firmware\n");
        return -1;
    }
    gpio_setup();
    mmio_write(CONTROL0, 0);
    mmio_write(CONTROL1, mmio_read(CONTROL1) | C1_SRST_HC);
    if (wait_reg(CONTROL1, C1_SRST_HC, 0, 100000) < 0)
        goto fail;
    mmio_write(CONTROL1, C1_CLK_INTLEN | (0xeu << 16));     // max data timeout
    if (set_clock(base, 400000) < 0)
        goto fail;
    mmio_write(INT_EN, 0xffffffff);     // flags visible in INTERRUPT; no IRQ is routed
    mmio_write(INT_MASK, 0xffffffff);

    rca = 0;
    cmd(CMD(0, R_NONE), 0);
    int v2 = cmd(CMD(8, R_48), 0x1aa) == 0 && (mmio_read(RESP0) & 0xfff) == 0x1aa;
    uint32_t ocr = 0;
    for (int i = 0; i < 100; i++) {     // up to ~1 s for power-up
        if (acmd(CMD(41, R_48NC), 0x00ff8000 | (v2 ? 0x40000000 : 0)) < 0)
            goto fail;
        if ((ocr = mmio_read(RESP0)) & 0x80000000)
            break;
        wait_us(10000);
    }
    if (!(ocr & 0x80000000))
        goto fail;
    block_addressing = (ocr >> 30) & 1;
    if (cmd(CMD(2, R_136), 0) < 0 || cmd(CMD(3, R_48), 0) < 0)
        goto fail;
    rca = mmio_read(RESP0) >> 16;
    if (set_clock(base, 25000000) < 0 || cmd(CMD(7, R_48B), rca << 16) < 0)
        goto fail;
    if (!block_addressing && cmd(CMD(16, R_48), 512) < 0)
        goto fail;
    ready = 1;
    kprintf("[SD] card ready: %s, EMMC base clock %u Hz\n",
            block_addressing ? "SDHC/SDXC" : "SDSC", base);
    return 0;
fail:
    kprintf("[SD] no card or init failed (INTERRUPT=%x STATUS=%x)\n",
            mmio_read(INTERRUPT), mmio_read(STATUS));
    return -1;
}

static int xfer(uint32_t lba, void *buf, int write)
{
    if (!ready || ((uintptr_t)buf & 3))
        return -1;
    if (wait_reg(STATUS, ST_DAT_INHIBIT, 0, 500000) < 0)
        return -1;
    mmio_write(BLKSIZECNT, (1u << 16) | 512);
    uint32_t arg = block_addressing ? lba : lba * 512;
    if (cmd(write ? CMD(24, R_48 | T_DATA) : CMD(17, R_48 | T_DATA | T_READ), arg) < 0)
        return -1;
    if (wait_int(write ? INT_WRITE_RDY : INT_READ_RDY, 500000) < 0)
        return -1;
    uint32_t *w = buf;
    for (int i = 0; i < 128; i++)
        if (write)
            mmio_write(DATA, w[i]);
        else
            w[i] = mmio_read(DATA);
    return wait_int(INT_DATA_DONE, 1000000);
}

int sd_read(uint32_t lba, void *buf) { return xfer(lba, buf, 0); }
int sd_write(uint32_t lba, const void *buf) { return xfer(lba, (void *)buf, 1); }
