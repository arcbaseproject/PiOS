#include "ulib.h"

// Busy loops without syscalls between prints: interleaving with another spin
// proves EL0 preemption.
int main(int argc, char **argv)
{
    const char *id = argc > 1 ? argv[1] : "?";
    for (int round = 1; round <= 3; round++) {
        for (volatile long i = 0; i < 20000000; i++)
            ;
        puts("[spin ");
        puts(id);
        puts("] round ");
        putnum(round);
        puts("\n");
    }
    return 0;
}
