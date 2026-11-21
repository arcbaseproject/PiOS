#pragma once
#include <stdint.h>

/* BCM2837 peripherals (physical 0x3F000000) and the ARM local peripherals
 * (physical 0x40000000), as seen through the kernel's TTBR1 mapping. */
#define PERIPHERAL_BASE 0xFFFFFF803F000000UL
#define LOCAL_BASE      0xFFFFFF8040000000UL

static inline void mmio_write(uintptr_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }
static inline uint32_t mmio_read(uintptr_t reg) { return *(volatile uint32_t *)reg; }
static inline void delay_cycles(uint32_t n) { while (n--) __asm__ volatile("nop"); }
