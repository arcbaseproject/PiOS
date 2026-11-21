// Identity map with 4 KiB granule, 39-bit VA (T0SZ=25), TTBR0 only.
// L1[0] -> L2 table of 2 MiB blocks: RAM as normal WB cacheable,
// 0x3F000000-0x3FFFFFFF as device. L1[1]: 1 GiB device block covering the
// local peripherals at 0x40000000. Everything else unmapped. All EL1-only
// (AP=00) and UXN; process tables share L1[0], L1[1] (see mm/vm.c).
#include "kernel.h"
#include "mm.h"

#define DESC_TABLE   0x3UL
#define DESC_BLOCK   0x1UL
#define ATTR_IDX(n)  ((uint64_t)(n) << 2)
#define SH_INNER     (3UL << 8)
#define AF           (1UL << 10)
#define PXN          (1UL << 53)
#define UXN          (1UL << 54)

#define MT_DEVICE 0     // MAIR attr0: Device-nGnRnE
#define MT_NORMAL 1     // MAIR attr1: Normal, inner/outer WB RW-allocate
#define MT_NC     2     // MAIR attr2: Normal non-cacheable (framebuffer: GPU sees writes)

#define NORMAL_BLOCK (DESC_BLOCK | ATTR_IDX(MT_NORMAL) | SH_INNER | AF | UXN)
#define DEVICE_BLOCK (DESC_BLOCK | ATTR_IDX(MT_DEVICE) | AF | PXN | UXN)

#define PERIPH_START 0x3F000000UL
#define BLOCK_2M     0x200000UL

uint64_t kernel_l1[512] __attribute__((aligned(4096)));
static uint64_t l2[512] __attribute__((aligned(4096)));

void mmu_init(uint64_t ram_end)
{
    if (ram_end > PERIPH_START)
        ram_end = PERIPH_START;
    for (uint64_t i = 0; i < 512; i++) {
        uint64_t pa = i * BLOCK_2M;
        if (pa >= PERIPH_START)
            l2[i] = pa | DEVICE_BLOCK;
        else if (pa + BLOCK_2M <= ram_end)
            l2[i] = pa | NORMAL_BLOCK;
        else
            l2[i] = 0;  // VideoCore-owned RAM: leave unmapped
    }
    kernel_l1[0] = (uintptr_t)l2 | DESC_TABLE;
    kernel_l1[1] = 0x40000000UL | DEVICE_BLOCK;

    write_sysreg(mair_el1, (0x00UL << (8 * MT_DEVICE)) | (0xFFUL << (8 * MT_NORMAL)) | (0x44UL << (8 * MT_NC)));
    write_sysreg(tcr_el1,
                 25UL               // T0SZ: 39-bit VA
                 | (1UL << 8)       // IRGN0 WB-WA
                 | (1UL << 10)      // ORGN0 WB-WA
                 | (3UL << 12)      // SH0 inner shareable
                 | (0UL << 14)      // TG0 4 KiB
                 | (1UL << 23)      // EPD1: no TTBR1 walks
                 | (0UL << 32));    // IPS 32-bit PA
    write_sysreg(ttbr0_el1, (uintptr_t)kernel_l1);
    __asm__ volatile("dsb ish; tlbi vmalle1; dsb ish; isb" ::: "memory");

    uint64_t sctlr = read_sysreg(sctlr_el1);
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);   // M, C, I
    write_sysreg(sctlr_el1, sctlr);
    __asm__ volatile("isb" ::: "memory");
}

// Map [pa, pa+size) of VideoCore RAM (below the peripherals) as non-cacheable,
// EL1-only, in 2 MiB blocks. Shared by every process through L1[0].
void mmu_map_nc(uint64_t pa, uint64_t size)
{
    for (uint64_t a = pa & ~(BLOCK_2M - 1); a < pa + size && a < PERIPH_START; a += BLOCK_2M)
        if (!l2[a / BLOCK_2M])
            l2[a / BLOCK_2M] = a | DESC_BLOCK | ATTR_IDX(MT_NC) | AF | PXN | UXN;
    __asm__ volatile("dsb ishst; tlbi vmalle1; dsb ish; isb" ::: "memory");
}
