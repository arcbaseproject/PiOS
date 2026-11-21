#include "exception.h"
#include "kernel.h"
#include "mailbox.h"
#include "mm.h"
#include "proc.h"
#include "sched.h"
#include "timer.h"
#include "uart.h"
#include "vfs.h"

extern char __stack_top[];

static void page_selftest(void)
{
    uint64_t before = page_free_count();
    void *a = page_alloc(), *b = page_alloc();
    if (!a || !b || a == b)
        panic("page selftest: bad alloc %p %p", a, b);
    page_free(b);
    if (page_alloc() != b)
        panic("page selftest: freed page not reused");
    page_free(b);
    page_free(a);
    if (page_free_count() != before)
        panic("page selftest: count mismatch");
}

void kernel_main(void)
{
    uart_init();
    kprintf("\nPiOS 0.5\n"
            "Architecture: AArch64\n"
            "Platform: Raspberry Pi Zero 2 W\n\n"
            "[BOOT] Kernel entry successful\n"
            "[BOOT] UART initialized\n"
            "[BOOT] Running at EL%lu\n", read_sysreg(CurrentEL) >> 2);

    exception_init();
    kprintf("[BOOT] Exception vectors installed\n");

#ifdef TEST_FAULT
    __asm__ volatile("brk #0x42");
#endif

    uint32_t mbase, msize;
    if (mbox_arm_memory(&mbase, &msize) != 0)
        panic("mailbox: ARM memory query failed");
    kprintf("[MEM] ARM RAM %x-%x (%u MiB)\n", mbase, mbase + msize, msize >> 20);
    if (mbase != 0)
        panic("unexpected ARM RAM base %x", mbase);

    mmu_init(msize);
    kprintf("[MEM] MMU on: identity map, caches enabled\n");

    page_init((uintptr_t)__stack_top, msize);
    page_selftest();
    kprintf("[MEM] Page allocator: %lu free pages (%lu MiB)\n",
            page_free_count(), page_free_count() * PAGE_SIZE >> 20);

    sched_init();
    vfs_init();
    proc_create_init();

    timer_init();
    irq_enable();
    kprintf("[KERNEL] Hello from PiOS! Interrupts on.\n");

    for (;;)
        __asm__ volatile("wfi");     // idle: runs only when nothing else is runnable
}
