#include "ulib.h"

// Touches kernel memory (the kernel image in the TTBR1 half, EL1-only): must be killed.
int main(void)
{
    puts("[segv] reading kernel address 0xffffff8000080000\n");
    volatile int *p = (volatile int *)0xffffff8000080000UL;
    int v = *p;
    puts("[segv] ERROR: read succeeded\n");
    return v;
}
