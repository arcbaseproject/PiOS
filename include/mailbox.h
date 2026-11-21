#pragma once
#include <stdint.h>
int mbox_arm_memory(uint32_t *base, uint32_t *size);
uint32_t mbox_clock_rate(uint32_t id);
int mbox_power_on(uint32_t id);    // 3 = USB HCD; waits until stable
int mbox_framebuffer(uint32_t w, uint32_t h, uint32_t *pa, uint32_t *size, uint32_t *pitch);
