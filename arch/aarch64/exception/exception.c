#include "exception.h"
#include "kernel.h"
#include "proc.h"
#include "uapi.h"
#include "sched.h"
#include "timer.h"
#include "uart.h"

extern char vectors[];

void exception_init(void)
{
    write_sysreg(vbar_el1, (uintptr_t)vectors);
    __asm__ volatile("isb");
}

static void dump(struct trap_frame *tf)
{
    for (int i = 0; i < 31; i++)
        kprintf("x%d=%lx%s", i, tf->x[i], (i % 4 == 3) ? "\n" : "  ");
    kprintf("\nELR=%lx SPSR=%lx\n", tf->elr, tf->spsr);
}

static const char *ec_name(unsigned ec)
{
    switch (ec) {
    case 0x00: return "unknown/undefined instruction";
    case 0x15: return "SVC (AArch64)";
    case 0x20: case 0x21: return "instruction abort";
    case 0x22: return "PC alignment fault";
    case 0x24: case 0x25: return "data abort";
    case 0x26: return "SP alignment fault";
    case 0x3c: return "BRK";
    default:   return "other";
    }
}

void handle_sync(struct trap_frame *tf)
{
    uint64_t esr = read_sysreg(esr_el1);
    unsigned ec = (esr >> 26) & 0x3f;
    kprintf("\n[FAULT] sync exception at EL1: %s\n", ec_name(ec));
    kprintf("ESR=%lx (EC=%x ISS=%lx) FAR=%lx\n", esr, ec, esr & 0x1ffffff,
            read_sysreg(far_el1));
    dump(tf);
    panic("unhandled synchronous exception");
}

// BCM2836-style per-core local interrupt controller (ARM Quad-A7 control block,
// also used on BCM2837/BCM2710). Core 0 IRQ source register.
#define CORE0_IRQ_SOURCE 0x40000060UL
#define SRC_CNTPNSIRQ    (1u << 1)
#define SRC_GPU          (1u << 8)   // BCM2835 controller, routed to core 0 by default

void handle_irq(struct trap_frame *tf)
{
    uint32_t src = *(volatile uint32_t *)CORE0_IRQ_SOURCE;
    int handled = 0;
    if ((src & SRC_GPU) && uart_irq_pending()) {
        uart_irq();
        handled = 1;
    }
    if (src & SRC_CNTPNSIRQ) {
        timer_irq();
        handled = 1;
    }
    if (handled)
        schedule();     // tick ends the timeslice; RX may have woken a reader
    else
        kprintf("[IRQ] spurious, source=%x\n", src);
    if ((tf->spsr & 0xf) == 0)      // returning to EL0t
        signal_deliver(tf);
}

// Exceptions from EL0: syscalls, or faults that kill the process.
void handle_el0_sync(struct trap_frame *tf)
{
    uint64_t esr = read_sysreg(esr_el1);
    unsigned ec = (esr >> 26) & 0x3f;
    if (ec == 0x15) {
        tf->x[0] = (uint64_t)syscall_dispatch(tf);
        signal_deliver(tf);
        return;
    }
    struct task *t = current_task();
    kprintf("[FAULT] pid %d (%s): %s, ESR=%lx FAR=%lx ELR=%lx: killed\n", t->pid, t->name,
            ec_name(ec), esr, read_sysreg(far_el1), tf->elr);
    do_exit(ec == 0x00 ? SIGILL : SIGSEGV);
}

void handle_bad(struct trap_frame *tf, uint64_t n)
{
    kprintf("\n[FAULT] unexpected vector %lu ESR=%lx\n", n, read_sysreg(esr_el1));
    dump(tf);
    panic("unexpected exception");
}
