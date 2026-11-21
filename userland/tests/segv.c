#include "ulib.h"

// Touches kernel memory (identity-mapped kernel image, EL1-only): must be killed.
int main(void)
{
    puts("[segv] reading kernel address 0x80000\n");
    volatile int *p = (volatile int *)0x80000;
    int v = *p;
    puts("[segv] ERROR: read succeeded\n");
    return v;
}
