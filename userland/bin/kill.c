#include "ulib.h"

static long num(const char *s)
{
    long v = 0;
    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

// kill [-SIG] pid...   Every signal terminates (no handlers yet).
int main(int argc, char **argv)
{
    int sig = SIGTERM, i = 1, rc = 0;
    if (argc > 1 && argv[1][0] == '-') {
        sig = (int)num(argv[1] + 1);
        i = 2;
    }
    if (sig < 1 || i >= argc) {
        eputs("usage: kill [-SIG] pid...\n");
        return 1;
    }
    for (; i < argc; i++) {
        long pid = num(argv[i]);
        int r = pid < 0 ? -EINVAL : kill((int)pid, sig);
        if (r < 0) {
            err("kill", argv[i], r);
            rc = 1;
        }
    }
    return rc;
}
