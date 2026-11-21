// Physical page allocator: singly linked free list threaded through free pages.
// shortcut: no contiguous multi-page allocation; switch to a bitmap/buddy when DMA or large buffers need it.
#include "kernel.h"
#include "mm.h"

struct free_page { struct free_page *next; };
static struct free_page *free_list;
static uint64_t nfree;

void page_init(uintptr_t start, uintptr_t end)
{
    start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    // Push highest first so allocation starts at low addresses.
    for (uintptr_t p = end; p > start; ) {
        p -= PAGE_SIZE;
        page_free((void *)p);
    }
}

void *page_alloc(void)
{
    struct free_page *p = free_list;
    if (!p)
        return 0;
    free_list = p->next;
    nfree--;
    memset(p, 0, PAGE_SIZE);
    return p;
}

void page_free(void *page)
{
    if ((uintptr_t)page & (PAGE_SIZE - 1))
        panic("page_free: unaligned %p", page);
    struct free_page *p = page;
    p->next = free_list;
    free_list = p;
    nfree++;
}

uint64_t page_free_count(void) { return nfree; }
