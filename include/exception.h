#pragma once
#include <stdint.h>

struct trap_frame {
    uint64_t x[31];
    uint64_t elr;
    uint64_t spsr;
    uint64_t sp_el0;
};
_Static_assert(sizeof(struct trap_frame) == 272, "must match vectors.S");

void exception_init(void);
