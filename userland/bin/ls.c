#include "ulib.h"

// One name per line; directories get a trailing '/'.
static int list(const char *path)
{
    struct stat st;
    int r = stat(path, &st);
    if (r < 0) {
        err("ls", path, r);
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        puts(path);
        puts("\n");
        return 0;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        err("ls", path, fd);
        return 1;
    }
    struct dirent de[16];
    long n;
    while ((n = getdents(fd, de, sizeof de)) > 0)
        for (long i = 0; i < n / (long)sizeof de[0]; i++) {
            if (de[i].d_name[0] == '.')
                continue;   // hidden, plus . and ..
            puts(de[i].d_name);
            puts(S_ISDIR(de[i].d_mode) ? "/\n" : "\n");
        }
    close(fd);
    if (n < 0) {
        err("ls", path, n);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return list(".");
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argc > 2) {
            puts(argv[i]);
            puts(":\n");
        }
        rc |= list(argv[i]);
    }
    return rc;
}
