#include "ulib.h"

// Files only; empty directories with -d (no recursive delete).
int main(int argc, char **argv)
{
    int rc = 0, dirs = argc > 1 && strcmp(argv[1], "-d") == 0;
    for (int i = 1 + dirs; i < argc; i++) {
        struct stat st;
        int r = stat(argv[i], &st);
        if (r == 0)
            r = S_ISDIR(st.st_mode) ? (dirs ? rmdir(argv[i]) : -EISDIR) : unlink(argv[i]);
        if (r < 0) {
            err("rm", argv[i], r);
            rc = 1;
        }
    }
    return rc;
}
