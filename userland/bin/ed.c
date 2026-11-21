// Line editor (ed-like subset). Lines are numbered from 1.
//   p          print all lines, numbered     N      print line N
//   a [N]      append after N (default: end) i N    insert before N
//   c N        replace line N                d N[,M] delete lines
//   w [file]   write                         q      quit (Q: quit, discard changes)
// a/i/c read text lines until a line containing only "."
#include "ulib.h"

static char **lines;
static long nlines, cap;
static int dirty;
static char fname[PATH_MAX];

static int insert(long at, const char *s, size_t len)
{
    if (nlines == cap) {
        long nc = cap ? cap * 2 : 64;
        char **nl = realloc(lines, (size_t)nc * sizeof *nl);
        if (!nl)
            return -1;
        lines = nl, cap = nc;
    }
    char *copy = malloc(len + 1);
    if (!copy)
        return -1;
    memcpy(copy, s, len);
    copy[len] = 0;
    memmove(lines + at + 1, lines + at, (size_t)(nlines - at) * sizeof *lines);
    lines[at] = copy;
    nlines++;
    dirty = 1;
    return 0;
}

static void delete(long from, long to)     // 0-based, inclusive
{
    for (long i = from; i <= to; i++)
        free(lines[i]);
    memmove(lines + from, lines + to + 1, (size_t)(nlines - to - 1) * sizeof *lines);
    nlines -= to - from + 1;
    dirty = 1;
}

// One line from fd without the newline; -1 at EOF. Buffers partial reads (pipes).
static long getline_fd(int fd, char *out, size_t max)
{
    static char buf[512];
    static size_t have;
    for (;;) {
        for (size_t i = 0; i < have; i++)
            if (buf[i] == '\n') {
                size_t n = i < max - 1 ? i : max - 1;
                memcpy(out, buf, n);
                out[n] = 0;
                memmove(buf, buf + i + 1, have - i - 1);
                have -= i + 1;
                return (long)n;
            }
        if (have == sizeof buf) {       // overlong line: split it
            size_t n = max - 1 < have ? max - 1 : have;
            memcpy(out, buf, n);
            out[n] = 0;
            memmove(buf, buf + n, have - n);
            have -= n;
            return (long)n;
        }
        long r = read(fd, buf + have, sizeof buf - have);
        if (r <= 0) {
            if (!have)
                return -1;
            buf[have++] = '\n';         // last line without newline
            continue;
        }
        have += (size_t)r;
    }
}

static int load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    char buf[1024], *acc = NULL;
    size_t alen = 0;
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        char *na = realloc(acc, alen + (size_t)n);
        if (!na) {
            close(fd);
            free(acc);
            return -ENOMEM;
        }
        acc = na;
        memcpy(acc + alen, buf, (size_t)n);
        alen += (size_t)n;
    }
    close(fd);
    size_t start = 0;
    for (size_t i = 0; i <= alen; i++)
        if (i == alen ? i > start : acc[i] == '\n') {
            insert(nlines, acc + start, i - start);
            start = i + 1;
        }
    free(acc);
    dirty = 0;
    return 0;
}

static void save(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        err("ed", path, fd);
        return;
    }
    long bytes = 0;
    for (long i = 0; i < nlines; i++) {
        size_t l = strlen(lines[i]);
        if (write(fd, lines[i], l) != (long)l || write(fd, "\n", 1) != 1) {
            err("ed", path, -EIO);
            close(fd);
            return;
        }
        bytes += (long)l + 1;
    }
    close(fd);
    dirty = 0;
    printf("%ld\n", bytes);
}

static void read_text(long at)
{
    char l[256];
    while (getline_fd(0, l, sizeof l) >= 0 && strcmp(l, ".") != 0)
        if (insert(at++, l, strlen(l)) < 0) {
            eputs("ed: out of memory\n");
            return;
        }
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        snprintf(fname, sizeof fname, "%s", argv[1]);
        int r = load(fname);
        if (r == -ENOENT)
            printf("%s: new file\n", fname);
        else if (r < 0)
            err("ed", fname, r);
        else
            printf("%ld lines\n", nlines);
    }
    char cmd[256];
    for (;;) {
        puts(": ");
        if (getline_fd(0, cmd, sizeof cmd) < 0)
            return 0;
        char *arg = cmd + 1;
        while (*arg == ' ')
            arg++;
        long n = atol(arg), m = n;
        char *comma = strchr(arg, ',');
        if (comma)
            m = atol(comma + 1);
        switch (cmd[0]) {
        case 0:
            break;
        case 'p':
            for (long i = 0; i < nlines; i++)
                printf("%4ld  %s\n", i + 1, lines[i]);
            break;
        case 'a':
            if (!*arg)
                n = nlines;
            if (n < 0 || n > nlines)
                goto bad;
            read_text(n);
            break;
        case 'i':
            if (n < 1 || n > nlines + 1)
                goto bad;
            read_text(n - 1);
            break;
        case 'c':
            if (n < 1 || n > nlines)
                goto bad;
            delete(n - 1, n - 1);
            read_text(n - 1);
            break;
        case 'd':
            if (n < 1 || m < n || m > nlines)
                goto bad;
            delete(n - 1, m - 1);
            break;
        case 'w':
            if (*arg)
                snprintf(fname, sizeof fname, "%s", arg);
            if (!fname[0])
                goto bad;
            save(fname);
            break;
        case 'q':
            if (dirty) {
                puts("unsaved changes (w to save, Q to discard)\n");
                break;
            }
            return 0;
        case 'Q':
            return 0;
        default:
            if (cmd[0] >= '0' && cmd[0] <= '9' && (n = atol(cmd)) >= 1 && n <= nlines) {
                printf("%s\n", lines[n - 1]);
                break;
            }
        bad:
            puts("?\n");
        }
    }
}
