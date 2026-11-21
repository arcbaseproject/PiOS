#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        eputs("usage: mv from to\n");
        return 1;
    }
    int r = rename(argv[1], argv[2]);
    if (r < 0) {
        err("mv", argv[1], r);
        return 1;
    }
    return 0;
}
