// Per-process user page tables (4 KiB pages, 3 levels from L1), loaded in
// TTBR0. Table entries hold physical addresses; the kernel reaches tables and
// pages through its TTBR1 alias (P2V).
#include "kernel.h"
#include "mm.h"

#define PTE_VALID   1UL
#define PTE_TABLE   3UL
#define PTE_PAGE    3UL
#define PTE_NORMAL  (1UL << 2)      // MAIR attr1
#define PTE_USER    (1UL << 6)      // AP[1]: EL0 access
#define PTE_SH      (3UL << 8)
#define PTE_AF      (1UL << 10)
#define PTE_NG      (1UL << 11)     // non-global, for ASIDs later
#define PTE_PXN     (1UL << 53)     // kernel never executes user pages

uint64_t vm_make_pte(uint64_t pa, uint64_t flags)
{
    return pa | flags | PTE_PAGE | PTE_NORMAL | PTE_USER | PTE_SH | PTE_AF | PTE_NG | PTE_PXN;
}

uint64_t *vm_new(void)
{
    return page_alloc();
}

// Returns the L3 entry for a user va, allocating tables if asked.
uint64_t *vm_walk(uint64_t *pgd, uint64_t va, int alloc)
{
    uint64_t *t = pgd;
    for (int shift = 30; shift > 12; shift -= 9) {
        uint64_t *e = &t[(va >> shift) & 511];
        if (!(*e & PTE_VALID)) {
            if (!alloc)
                return NULL;
            void *n = page_alloc();
            if (!n)
                return NULL;
            *e = V2P(n) | PTE_TABLE;
        }
        t = P2V(PTE_ADDR(*e));
    }
    return &t[(va >> 12) & 511];
}

void *vm_alloc_page(uint64_t *pgd, uint64_t va, uint64_t flags)
{
    uint64_t *pte = vm_walk(pgd, va, 1);
    if (!pte)
        return NULL;
    void *pa = page_alloc();
    if (pa)
        *pte = vm_make_pte(V2P(pa), flags);
    return pa;
}

void vm_free(uint64_t *pgd)
{
    for (int i = 0; i < 512; i++) {
        if (!(pgd[i] & PTE_VALID))
            continue;
        uint64_t *l2 = P2V(PTE_ADDR(pgd[i]));
        for (int j = 0; j < 512; j++) {
            if (!(l2[j] & PTE_VALID))
                continue;
            uint64_t *l3 = P2V(PTE_ADDR(l2[j]));
            for (int k = 0; k < 512; k++)
                if (l3[k] & PTE_VALID)
                    page_free(P2V(PTE_ADDR(l3[k])));
            page_free(l3);
        }
        page_free(l2);
    }
    page_free(pgd);
}

// shortcut: eager full copy on fork, no copy-on-write; add COW if fork+exec cost matters.
uint64_t *vm_copy(uint64_t *src)
{
    uint64_t *dst = vm_new();
    if (!dst)
        return NULL;
    for (int i = 0; i < 512; i++) {
        if (!(src[i] & PTE_VALID))
            continue;
        uint64_t *l2 = P2V(PTE_ADDR(src[i]));
        for (int j = 0; j < 512; j++) {
            if (!(l2[j] & PTE_VALID))
                continue;
            uint64_t *l3 = P2V(PTE_ADDR(l2[j]));
            for (int k = 0; k < 512; k++) {
                if (!(l3[k] & PTE_VALID))
                    continue;
                uint64_t va = ((uint64_t)i << 30) | ((uint64_t)j << 21) | ((uint64_t)k << 12);
                uint64_t *pte = vm_walk(dst, va, 1);
                void *pa = pte ? page_alloc() : NULL;
                if (!pa) {
                    vm_free(dst);
                    return NULL;
                }
                memcpy(pa, P2V(PTE_ADDR(l3[k])), PAGE_SIZE);
                dcache_clean_pou((uintptr_t)pa, PAGE_SIZE);
                *pte = V2P(pa) | (l3[k] & ~PTE_ADDR(~0UL));
            }
        }
    }
    icache_inval_all();
    return dst;
}

// 1 if [va, va+len) lies in user space and every page is EL0-accessible
// (and writable if asked). Kernel then dereferences user va directly.
int vm_check(uint64_t *pgd, uint64_t va, uint64_t len, int write)
{
    if (!pgd || va < USER_BASE || va >= USER_TOP || len > USER_TOP - va)
        return 0;
    for (uint64_t p = va & ~(PAGE_SIZE - 1); p < va + len; p += PAGE_SIZE) {
        uint64_t *pte = vm_walk(pgd, p, 0);
        if (!pte || !(*pte & PTE_VALID) || !(*pte & PTE_USER) || (write && (*pte & VM_RO)))
            return 0;
    }
    return 1;
}

// shortcut: full TLB flush on every switch, no ASIDs; add ASIDs when switch cost shows up.
void vm_activate(uint64_t *pgd)
{
    uint64_t t = pgd ? V2P(pgd) : empty_ttbr0();
    __asm__ volatile("dsb ishst; msr ttbr0_el1, %0; isb; tlbi vmalle1; dsb ish; isb"
                     :: "r"(t) : "memory");
}

// Make data written through the kernel alias visible to instruction fetch.
void dcache_clean_pou(uintptr_t addr, size_t len)
{
    uint64_t line = 4UL << ((read_sysreg(ctr_el0) >> 16) & 0xf);
    for (uintptr_t p = addr & ~(line - 1); p < addr + len; p += line)
        __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
}

void icache_inval_all(void)
{
    __asm__ volatile("ic ialluis; dsb ish; isb" ::: "memory");
}

// Unmap one user page (if mapped) and free it.
void vm_unmap(uint64_t *pgd, uint64_t va)
{
    uint64_t *pte = vm_walk(pgd, va, 0);
    if (!pte || !(*pte & PTE_VALID))
        return;
    page_free(P2V(PTE_ADDR(*pte)));
    *pte = 0;
    __asm__ volatile("dsb ishst; tlbi vmalle1; dsb ish; isb" ::: "memory");
}
