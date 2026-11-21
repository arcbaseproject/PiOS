#pragma once
#include <stdint.h>
#define HZ 100
void timer_init(void);
void timer_irq(void);
uint64_t timer_ticks(void);
