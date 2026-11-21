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
    char buf[1024] __attribute__((aligned(8)));
    long n;
    while ((n = getdents64(fd, buf, sizeof buf)) > 0)
        for (long off = 0; off < n; off += ((struct dirent64 *)(buf + off))->d_reclen) {
            struct dirent64 *de = (struct dirent64 *)(buf + off);
            if (de->d_name[0] == '.')
                continue;   // hidden, plus . and ..
            puts(de->d_name);
            puts(de->d_type == DT_DIR ? "/\n" : "\n");
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
