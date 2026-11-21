#include "ulib.h"

// Two copies run concurrently with different increments; FP registers that
// leak between processes on preemption make the sums wrong.
int main(int argc, char **argv)
{
    double step = argc > 1 && argv[1][0] == 'b' ? 0.25 : 0.5;
    volatile double x = 0;
    for (long i = 0; i < 4000000; i++)
        x += step;
    return (long)(x / step) == 4000000 && x == 4000000 * step ? 0 : 1;
}
