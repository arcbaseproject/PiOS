#pragma once
#include <stdint.h>
#define HZ 100
void timer_init(void);
void timer_irq(void);
uint64_t timer_ticks(void);
long sys_nanosleep(uint64_t ureq);
long sys_clock_gettime(int clk, uint64_t uts);
