#pragma once
#include <stddef.h>
#include <stdint.h>
#define PAGE_SIZE 4096UL

/* Kernel half (TTBR1): every physical address pa is mapped at KERNEL_VA + pa
 * (RAM, VideoCore RAM on demand, peripherals). User space lives in TTBR0. */
#define KERNEL_VA 0xFFFFFF8000000000UL
#define P2V(pa) ((void *)((uintptr_t)(pa) + KERNEL_VA))
#define V2P(va) ((uintptr_t)(va) - KERNEL_VA)

/* User address space: one L1 slot (1 GiB) per process. */
#define USER_BASE        0x80000000UL
#define USER_TOP         0xC0000000UL
#define USER_STACK_PAGES 64    // 256 KiB, mapped eagerly (Lua's parser and VM recurse deeply)
#define USER_STACK_TOP   USER_TOP
#define USER_STACK_BASE  (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)

/* Page flags for vm_map */
#define VM_RO   (1UL << 7)      /* AP[2]: read-only */
#define VM_NX   (1UL << 54)     /* UXN: not executable at EL0 */

extern uint64_t kernel_l1[512], kernel_l2[512];

void mmu_init(uint64_t ram_end);
void mmu_map_nc(uint64_t pa, uint64_t size);    // VideoCore RAM, e.g. framebuffer
uint64_t empty_ttbr0(void);                     // physical address of an empty user table
void page_init(uintptr_t start, uintptr_t end);
void *page_alloc(void);
void page_free(void *p);
uint64_t page_free_count(void);

uint64_t *vm_new(void);
void vm_free(uint64_t *pgd);
uint64_t *vm_copy(uint64_t *src);
uint64_t *vm_walk(uint64_t *pgd, uint64_t va, int alloc);
void *vm_alloc_page(uint64_t *pgd, uint64_t va, uint64_t flags);
int vm_check(uint64_t *pgd, uint64_t va, uint64_t len, int write);
void vm_activate(uint64_t *pgd);
uint64_t vm_make_pte(uint64_t pa, uint64_t flags);
void dcache_clean_pou(uintptr_t addr, size_t len);
void icache_inval_all(void);

#define PTE_ADDR(e) ((e) & 0x0000FFFFFFFFF000UL)

void *kmalloc(size_t size);     // zeroed; max 2032 bytes
void kfree(void *p);
