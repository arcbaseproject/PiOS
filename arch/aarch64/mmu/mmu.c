// Kernel mapping in TTBR1 (4 KiB granule, 39-bit VA): KERNEL_VA + pa for
// L1[0] -> L2 table of 2 MiB blocks: RAM as normal WB cacheable,
// 0x3F000000-0x3FFFFFFF as device. L1[1]: 1 GiB device block covering the
// local peripherals at 0x40000000. All EL1-only (AP=00) and UXN. start.S
// builds it and turns the MMU on; TTBR0 holds only user mappings.
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

uint64_t kernel_l1[512] __attribute__((aligned(4096)));   // TTBR1; filled by start.S
uint64_t kernel_l2[512] __attribute__((aligned(4096)));
static uint64_t empty_l1[512] __attribute__((aligned(4096)));  // TTBR0 for kernel threads

// start.S mapped all of 0-0x3F000000 as RAM. Now that the ARM/VideoCore split
// is known, unmap VideoCore RAM and drop the boot identity map in TTBR0.
void mmu_init(uint64_t ram_end)
{
    if (ram_end > PERIPH_START)
        ram_end = PERIPH_START;
    for (uint64_t i = 0; i < 512; i++) {
        uint64_t pa = i * BLOCK_2M;
        if (pa < PERIPH_START && pa + BLOCK_2M > ram_end)
            kernel_l2[i] = 0;   // VideoCore-owned RAM: leave unmapped
    }
    write_sysreg(ttbr0_el1, V2P(empty_l1));
    __asm__ volatile("dsb ishst; tlbi vmalle1; dsb ish; isb" ::: "memory");
}

uint64_t empty_ttbr0(void) { return V2P(empty_l1); }

// Map [pa, pa+size) of VideoCore RAM (below the peripherals) as non-cacheable,
// EL1-only, in 2 MiB blocks. Shared by every process through L1[0].
void mmu_map_nc(uint64_t pa, uint64_t size)
{
    for (uint64_t a = pa & ~(BLOCK_2M - 1); a < pa + size && a < PERIPH_START; a += BLOCK_2M)
        if (!kernel_l2[a / BLOCK_2M])
            kernel_l2[a / BLOCK_2M] = a | DESC_BLOCK | ATTR_IDX(MT_NC) | AF | PXN | UXN;
    __asm__ volatile("dsb ishst; tlbi vmalle1; dsb ish; isb" ::: "memory");
}
