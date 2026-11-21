// lwIP port for the PiOS kernel: types and diagnostics.
#pragma once
#include "kernel.h"

#define LWIP_NO_INTTYPES_H 1
#define X8_F  "x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"
#define LWIP_NO_CTYPE_H 1
#define LWIP_NO_UNISTD_H 1
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PLATFORM_DIAG(x) do { kprintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) panic("lwip: %s (%s:%d)", x, __FILE__, __LINE__)
#define LWIP_RAND() net_rand()
unsigned net_rand(void);
