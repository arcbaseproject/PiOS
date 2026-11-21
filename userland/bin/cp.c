#include "ulib.h"

static char buf[4096];

// shortcut: destination must be a file path, not a directory; add dir/basename when needed.
int main(int argc, char **argv)
{
    if (argc != 3) {
        eputs("usage: cp from to\n");
        return 1;
    }
    int in = open(argv[1], O_RDONLY);
    if (in < 0) {
        err("cp", argv[1], in);
        return 1;
    }
    int out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        err("cp", argv[2], out);
        return 1;
    }
    long n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        long w = write(out, buf, (size_t)n);
        if (w != n) {
            err("cp", argv[2], w < 0 ? w : -ENOSPC);
            return 1;
        }
    }
    if (n < 0) {
        err("cp", argv[1], n);
        return 1;
    }
    return 0;
}
