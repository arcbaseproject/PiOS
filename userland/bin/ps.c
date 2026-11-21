#include "ulib.h"

static void col(long v, int width)
{
    int digits = 1;
    for (long x = v; x >= 10; x /= 10)
        digits++;
    while (digits++ < width)
        puts(" ");
    putnum(v);
}

int main(void)
{
    static struct procinfo p[64];
    int n = getprocs(p, 64);
    puts("  PID  PPID  PGID S NAME\n");
    for (int i = 0; i < n; i++) {
        col(p[i].pid, 5);
        col(p[i].ppid, 6);
        col(p[i].pgid, 6);
        char s[4] = { ' ', p[i].state, ' ', 0 };
        puts(s);
        puts(p[i].name);
        puts("\n");
    }
    return n < 0;
}
