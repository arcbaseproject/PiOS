#include "ulib.h"

int main(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long s = ts.tv_sec;
    printf("up %ld:%02ld:%02ld\n", s / 3600, s / 60 % 60, s % 60);
    return 0;
}
