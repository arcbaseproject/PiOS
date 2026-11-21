#include "ulib.h"

int main(void)
{
    char buf[PATH_MAX];
    long r = getcwd(buf, sizeof buf);
    if (r < 0) {
        err("pwd", 0, r);
        return 1;
    }
    puts(buf);
    puts("\n");
    return 0;
}
