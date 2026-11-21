// Small-object allocator: power-of-two classes 32..2048 bytes carved from pages.
// 16-byte header stores the class so kfree needs no size.
// shortcut: freed objects return to their class list, never to the page allocator.
#include "kernel.h"
#include "mm.h"

#define MIN_SHIFT 5
#define NCLASS    7     // 32 .. 2048
#define HDR       16

struct obj { struct obj *next; };
static struct obj *freelist[NCLASS];

void *kmalloc(size_t size)
{
    int c = 0;
    while (c < NCLASS && (1UL << (c + MIN_SHIFT)) < size + HDR)
        c++;
    if (c == NCLASS)
        panic("kmalloc: %lu bytes too large, use page_alloc", (uint64_t)size);
    if (!freelist[c]) {
        uint8_t *page = page_alloc();
        if (!page)
            return NULL;
        size_t sz = 1UL << (c + MIN_SHIFT);
        for (size_t off = 0; off + sz <= PAGE_SIZE; off += sz) {
            struct obj *o = (struct obj *)(page + off);
            o->next = freelist[c];
            freelist[c] = o;
        }
    }
    struct obj *o = freelist[c];
    freelist[c] = o->next;
    *(uint64_t *)o = (uint64_t)c;
    void *p = (uint8_t *)o + HDR;
    memset(p, 0, (1UL << (c + MIN_SHIFT)) - HDR);
    return p;
}

void kfree(void *p)
{
    if (!p)
        return;
    struct obj *o = (struct obj *)((uint8_t *)p - HDR);
    uint64_t c = *(uint64_t *)o;
    if (c >= NCLASS)
        panic("kfree: bad pointer %p", p);
    o->next = freelist[c];
    freelist[c] = o;
}
