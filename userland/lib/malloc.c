// K&R-style first-fit allocator with coalescing, heap grown via brk().
// Small blocks (<= SMALL units) are recycled through per-size free lists in
// O(1) instead: K&R free() walks the address-ordered list, which goes
// quadratic when a program frees many interleaved small blocks (Lua's GC).
// shortcut: small blocks never return to the coalescing arena, so memory used
// by one size class is not reusable by another.
#include "ulib.h"

typedef union header {
    struct {
        union header *next;     // next free block (circular list)
        size_t units;           // block size in header units, header included
    } s;
    long double align;          // 16-byte alignment for every block
} header;

static header base, *freep;

#define SMALL 33                // units, header included: requests up to 512 bytes
static header *small[SMALL + 1];

void *sbrk(long inc)
{
    static char *cur;
    if (!cur)
        cur = (char *)brk(0);
    char *old = cur;
    if (inc && brk(cur + inc) != (long)(cur + inc))
        return (void *)-1;
    cur += inc;
    return old;
}

static void arena_free(header *b);

void free(void *p)
{
    if (!p)
        return;
    header *b = (header *)p - 1;
    if (b->s.units <= SMALL) {
        b->s.next = small[b->s.units];
        small[b->s.units] = b;
        return;
    }
    arena_free(b);
}

static void arena_free(header *b)
{
    header *q;
    for (q = freep; !(b > q && b < q->s.next); q = q->s.next)
        if (q >= q->s.next && (b > q || b < q->s.next))
            break;              // at either end of the arena
    if (b + b->s.units == q->s.next) {
        b->s.units += q->s.next->s.units;
        b->s.next = q->s.next->s.next;
    } else {
        b->s.next = q->s.next;
    }
    if (q + q->s.units == b) {
        q->s.units += b->s.units;
        q->s.next = b->s.next;
    } else {
        q->s.next = b;
    }
    freep = q;
}

static header *morecore(size_t units)
{
    if (units < 1024)
        units = 1024;           // grow in 16 KiB steps
    char *p = sbrk((long)(units * sizeof(header)));
    if (p == (char *)-1)
        return NULL;
    header *h = (header *)p;
    h->s.units = units;
    arena_free(h);
    return freep;
}

void *malloc(size_t n)
{
    if (n == 0 || n > (1UL << 30))
        return NULL;
    size_t units = (n + sizeof(header) - 1) / sizeof(header) + 1;
    if (units <= SMALL && small[units]) {
        header *b = small[units];
        small[units] = b->s.next;
        return b + 1;
    }
    if (!freep) {
        base.s.next = freep = &base;
        base.s.units = 0;
    }
    header *prev = freep;
    for (header *p = prev->s.next;; prev = p, p = p->s.next) {
        if (p->s.units >= units) {
            if (p->s.units == units) {
                prev->s.next = p->s.next;
            } else {
                p->s.units -= units;    // hand out the tail
                p += p->s.units;
                p->s.units = units;
            }
            freep = prev;
            return p + 1;
        }
        if (p == freep && !(p = morecore(units)))
            return NULL;
    }
}

void *calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size)
        return NULL;
    void *p = malloc(n * size);
    return p ? memset(p, 0, n * size) : NULL;
}

void *realloc(void *old, size_t n)
{
    if (!old)
        return malloc(n);
    if (!n) {
        free(old);
        return NULL;
    }
    size_t have = (((header *)old - 1)->s.units - 1) * sizeof(header);
    if (n <= have)
        return old;
    void *p = malloc(n);
    if (p) {
        memcpy(p, old, have);
        free(old);
    }
    return p;
}
