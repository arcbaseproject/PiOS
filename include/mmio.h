#pragma once
#include <stdint.h>

#define PERIPHERAL_BASE 0x3F000000UL   /* BCM2837/BCM2710 ARM-side peripheral base */

static inline void mmio_write(uintptr_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }
static inline uint32_t mmio_read(uintptr_t reg) { return *(volatile uint32_t *)reg; }
static inline void delay_cycles(uint32_t n) { while (n--) __asm__ volatile("nop"); }
