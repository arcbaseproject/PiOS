// ARM generic timer, EL1 non-secure physical timer (CNTP), routed to core 0
// IRQ through the local interrupt controller at 0x40000000.
#include "kernel.h"
#include "timer.h"

#define CORE0_TIMER_IRQCNTL 0x40000040UL
#define CNTPNSIRQ_IRQ       (1u << 1)

static uint64_t interval;
static volatile uint64_t ticks;

void timer_init(void)
{
    uint64_t freq = read_sysreg(cntfrq_el0);   // set by firmware (19.2 MHz on BCM2710)
    if (freq == 0)
        panic("CNTFRQ_EL0 is 0, firmware did not set timer frequency");
    interval = freq / HZ;
    write_sysreg(cntp_tval_el0, interval);
    write_sysreg(cntp_ctl_el0, 1);              // ENABLE=1, IMASK=0
    *(volatile uint32_t *)CORE0_TIMER_IRQCNTL = CNTPNSIRQ_IRQ;
    kprintf("[TIMER] generic timer %lu Hz, tick %d Hz\n", freq, HZ);
}

void timer_irq(void)
{
    write_sysreg(cntp_tval_el0, interval);      // re-arm; also clears the condition
    ticks++;
}

uint64_t timer_ticks(void) { return ticks; }
