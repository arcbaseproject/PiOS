// ELF64 loader for static AArch64 executables (ET_EXEC), read through the VFS
// into a user page table.
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "uapi.h"
#include "vfs.h"

struct elf64_ehdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

#define PT_LOAD    1
#define PF_X       1
#define PF_W       2
#define EM_AARCH64 183

#define MAX_PHDRS 8

static int rd(struct inode *ino, void *buf, uint64_t len, uint64_t off)
{
    return ino->ops->read(ino, buf, len, off) == (long)len ? 0 : -ENOEXEC;
}

int elf_load(uint64_t *pgd, struct inode *ino, struct elf_info *info)
{
    struct elf64_ehdr eh;
    struct elf64_phdr phs[MAX_PHDRS];
    uint64_t size = ino->size;
    if (rd(ino, &eh, sizeof eh, 0) < 0 || eh.ident[0] != 0x7f || eh.ident[1] != 'E' ||
        eh.ident[2] != 'L' || eh.ident[3] != 'F' || eh.ident[4] != 2 /* 64-bit */ ||
        eh.ident[5] != 1 /* LE */ || eh.type != 2 /* EXEC */ || eh.machine != EM_AARCH64 ||
        eh.phentsize != sizeof(struct elf64_phdr) || eh.phnum > MAX_PHDRS ||
        rd(ino, phs, eh.phnum * sizeof *phs, eh.phoff) < 0)
        return -ENOEXEC;
    if (eh.entry < USER_BASE || eh.entry >= USER_STACK_BASE)
        return -ENOEXEC;
    uint64_t *top = &info->top;
    *top = USER_BASE;
    info->entry = eh.entry;
    info->phnum = eh.phnum;
    info->phdr = 0;

    for (int i = 0; i < eh.phnum; i++) {
        const struct elf64_phdr *ph = &phs[i];
        if (ph->type != PT_LOAD || ph->memsz == 0)
            continue;
        uint64_t va0 = ph->vaddr, end = ph->vaddr + ph->memsz;
        if (ph->filesz > ph->memsz || ph->offset > size || ph->filesz > size - ph->offset ||
            va0 < USER_BASE || end < va0 || end > USER_STACK_BASE)
            return -ENOEXEC;
        uint64_t flags = (ph->flags & PF_W ? 0 : VM_RO) | (ph->flags & PF_X ? 0 : VM_NX);
        if (eh.phoff >= ph->offset && eh.phoff - ph->offset < ph->filesz)
            info->phdr = va0 + (eh.phoff - ph->offset);    // for AT_PHDR (musl finds PT_TLS)
        if (end > *top)
            *top = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (uint64_t va = va0 & ~(PAGE_SIZE - 1); va < end; va += PAGE_SIZE) {
            uint64_t *pte = vm_walk(pgd, va, 1);
            if (!pte)
                return -ENOMEM;
            if (*pte & 1) {
                *pte &= ~(VM_RO | VM_NX) | flags;   // page shared by two segments: widen
            } else {
                void *pa = page_alloc();
                if (!pa)
                    return -ENOMEM;
                *pte = vm_make_pte(V2P(pa), flags);
            }
            uint8_t *page = P2V(PTE_ADDR(*pte));
            uint64_t lo = va > va0 ? va : va0;
            uint64_t fend = va0 + ph->filesz;
            uint64_t hi = va + PAGE_SIZE < fend ? va + PAGE_SIZE : fend;
            if (lo < hi && rd(ino, page + (lo - va), hi - lo, ph->offset + (lo - va0)) < 0)
                return -ENOEXEC;
            dcache_clean_pou((uintptr_t)page, PAGE_SIZE);
        }
    }
    icache_inval_all();
    return 0;
}
