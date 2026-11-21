#include "ulib.h"

// sleep SECONDS[.FRACTION]
int main(int argc, char **argv)
{
    if (argc != 2) {
        eputs("usage: sleep seconds\n");
        return 1;
    }
    struct timespec ts = { atol(argv[1]), 0 };
    const char *dot = strchr(argv[1], '.');
    if (dot)
        for (long scale = 100000000, i = 1; dot[i] >= '0' && dot[i] <= '9' && scale; i++, scale /= 10)
            ts.tv_nsec += (dot[i] - '0') * scale;
    if (ts.tv_sec < 0) {
        eputs("sleep: invalid time\n");
        return 1;
    }
    return nanosleep(&ts, 0) < 0;
}
