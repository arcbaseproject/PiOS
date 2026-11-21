#include "ulib.h"

int main(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = mkdir(argv[i], 0755);
        if (r < 0) {
            err("mkdir", argv[i], r);
            rc = 1;
        }
    }
    return rc;
}
