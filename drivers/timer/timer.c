// ARM generic timer, EL1 non-secure physical timer (CNTP), routed to core 0
// IRQ through the local interrupt controller at 0x40000000.
#include "kernel.h"
#include "mm.h"
#include "sched.h"
#include "timer.h"
#include "uapi.h"

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
    wakeup((void *)&ticks);     // nanosleep()ers recheck their deadline
}

long sys_nanosleep(uint64_t ureq)
{
    struct task *t = current_task();
    if (!vm_check(t->pgd, ureq, sizeof(struct timespec), 0))
        return -EFAULT;
    const struct timespec *ts = (const struct timespec *)ureq;
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)
        return -EINVAL;
    // Round up to whole ticks, plus one so we never return early.
    uint64_t n = (uint64_t)ts->tv_sec * HZ + ((uint64_t)ts->tv_nsec * HZ + 999999999) / 1000000000;
    uint64_t end = ticks + n + (n ? 1 : 0);
    while (ticks < end)
        if (sleep_on((void *)&ticks))
            return -EINTR;
    return 0;
}

long sys_clock_gettime(int clk, uint64_t uts)
{
    if (clk != CLOCK_MONOTONIC)
        return -EINVAL;
    if (!vm_check(current_task()->pgd, uts, sizeof(struct timespec), 1))
        return -EFAULT;
    uint64_t c = read_sysreg(cntpct_el0), f = read_sysreg(cntfrq_el0);
    struct timespec *ts = (struct timespec *)uts;
    ts->tv_sec = (int64_t)(c / f);
    ts->tv_nsec = (int64_t)((c % f) * 1000000000 / f);
    return 0;
}

uint64_t timer_ticks(void) { return ticks; }
