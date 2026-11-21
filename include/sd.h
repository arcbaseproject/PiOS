#pragma once
#include <stdint.h>
int sd_init(void);                              // 0 = card ready
int sd_read(uint32_t lba, void *buf);           // one 512-byte block, buf 4-byte aligned
int sd_write(uint32_t lba, const void *buf);
