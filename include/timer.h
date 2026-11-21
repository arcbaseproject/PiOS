#pragma once
#include <stdint.h>
#define HZ 100
void timer_init(void);
void timer_irq(void);
uint64_t timer_ticks(void);
void msleep(unsigned ms);           // kernel threads: sleep at least ms (10 ms granularity)
void udelay(unsigned us);           // busy-wait on the counter
long sys_nanosleep(uint64_t ureq);
long sys_clock_gettime(int clk, uint64_t uts);
