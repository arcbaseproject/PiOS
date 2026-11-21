// Buffered stdio over PiOS file descriptors. One buffer per FILE, used for
// either reading or writing at a time (switching flushes or discards it).
#include "ulib.h"
#include "errno.h"
#include "stdio.h"
#include "string.h"

#define F_READ   1
#define F_WRITE  2
#define F_EOF    4
#define F_ERR    8
#define F_APPEND 16
#define NFILES   16

struct FILE {
    int fd, flags, mode;            // mode: _IOFBF / _IOLBF / _IONBF
    int used;
    size_t rpos, rlen;              // read buffer window
    size_t wlen;                    // bytes waiting to be written
    int unget;                      // pushed-back char or EOF
    unsigned char buf[BUFSIZ];
};

static FILE files[NFILES] = {
    { .fd = 0, .flags = F_READ, .mode = _IOLBF, .used = 1, .unget = EOF },
    { .fd = 1, .flags = F_WRITE, .mode = _IOLBF, .used = 1, .unget = EOF },
    { .fd = 2, .flags = F_WRITE, .mode = _IONBF, .used = 1, .unget = EOF },
};
FILE *stdin = &files[0], *stdout = &files[1], *stderr = &files[2];
int errno;

static int fail(FILE *f, long r)
{
    errno = (int)-r;
    f->flags |= F_ERR;
    return EOF;
}

static int flush_write(FILE *f)
{
    size_t done = 0;
    while (done < f->wlen) {
        long r = write(f->fd, f->buf + done, f->wlen - done);
        if (r <= 0) {
            f->wlen = 0;
            return fail(f, r ? r : -EIO);
        }
        done += (size_t)r;
    }
    f->wlen = 0;
    return 0;
}

// Drop buffered input, moving the fd back to the logical position.
static void drop_read(FILE *f)
{
    long ahead = (long)(f->rlen - f->rpos) + (f->unget != EOF);
    if (ahead)
        lseek(f->fd, -ahead, SEEK_CUR);     // fails harmlessly on pipes/console
    f->rpos = f->rlen = 0;
    f->unget = EOF;
}

static void flush_all(void)
{
    for (int i = 0; i < NFILES; i++)
        if (files[i].used && files[i].wlen)
            flush_write(&files[i]);
}

static FILE *setup(FILE *f, int fd, int flags)
{
    f->fd = fd, f->flags = flags, f->used = 1;
    f->mode = _IOFBF;
    f->rpos = f->rlen = f->wlen = 0;
    f->unget = EOF;
    __exit_hook = flush_all;
    return f;
}

static int parse_mode(const char *m, int *oflags)
{
    int plus = strchr(m, '+') != NULL;
    switch (m[0]) {
    case 'r': *oflags = plus ? O_RDWR : O_RDONLY; return plus ? F_READ | F_WRITE : F_READ;
    case 'w': *oflags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; return plus ? F_READ | F_WRITE : F_WRITE;
    case 'a': *oflags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
              return (plus ? F_READ | F_WRITE : F_WRITE) | F_APPEND;
    }
    return 0;
}

FILE *fopen(const char *path, const char *mode)
{
    int of, fl = parse_mode(mode, &of);
    if (!fl) {
        errno = EINVAL;
        return NULL;
    }
    FILE *f = NULL;
    for (int i = 3; i < NFILES && !f; i++)
        if (!files[i].used)
            f = &files[i];
    if (!f) {
        errno = EMFILE;
        return NULL;
    }
    int fd = open(path, of, 0644);
    if (fd < 0) {
        errno = -fd;
        return NULL;
    }
    return setup(f, fd, fl);
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    fflush(f);
    int of, fl = parse_mode(mode, &of);
    if (!path || !fl) {
        errno = EINVAL;
        return NULL;
    }
    int fd = open(path, of, 0644);
    if (fd < 0) {
        errno = -fd;
        return NULL;
    }
    if (fd != f->fd) {          // keep the same fd number (stdin stays 0)
        dup2(fd, f->fd);
        close(fd);
    }
    return setup(f, f->fd, fl);
}

int fclose(FILE *f)
{
    int r = fflush(f);
    if (close(f->fd) < 0)
        r = EOF;
    f->used = 0;
    return r;
}

int fflush(FILE *f)
{
    if (!f) {
        flush_all();
        return 0;
    }
    if (f->wlen)
        return flush_write(f);
    if (f->rlen || f->unget != EOF)
        drop_read(f);
    return 0;
}

static int fill(FILE *f)
{
    if (f->wlen && flush_write(f))
        return EOF;
    if (f == stdin)
        fflush(stdout);         // show the prompt before blocking
    long r = read(f->fd, f->buf, f->mode == _IONBF ? 1 : sizeof f->buf);
    if (r <= 0) {
        if (r == 0)
            f->flags |= F_EOF;
        else
            fail(f, r);
        return EOF;
    }
    f->rpos = 0;
    f->rlen = (size_t)r;
    return 0;
}

int fgetc(FILE *f)
{
    if (f->unget != EOF) {
        int c = f->unget;
        f->unget = EOF;
        return c;
    }
    if (f->rpos == f->rlen && fill(f))
        return EOF;
    return f->buf[f->rpos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
    if (c == EOF || f->unget != EOF)
        return EOF;
    f->unget = (unsigned char)c;
    f->flags &= ~F_EOF;
    return c;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f)
{
    unsigned char *p = buf;
    size_t want = size * n, got = 0;
    while (got < want) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        p[got++] = (unsigned char)c;
        size_t k = f->rlen - f->rpos;       // copy the rest of the buffer in one go
        if (k > want - got)
            k = want - got;
        memcpy(p + got, f->buf + f->rpos, k);
        f->rpos += k, got += k;
    }
    return size ? got / size : 0;
}

char *fgets(char *s, int n, FILE *f)
{
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (!i)
        return NULL;
    s[i] = 0;
    return s;
}

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    const unsigned char *p = buf;
    size_t len = size * n;
    if (f->rlen || f->unget != EOF)
        drop_read(f);
    for (size_t i = 0; i < len; i++) {
        f->buf[f->wlen++] = p[i];
        if (f->wlen == sizeof f->buf || (f->mode == _IOLBF && p[i] == '\n'))
            if (flush_write(f))
                return size ? i / size : 0;
    }
    if (f->mode == _IONBF && f->wlen && flush_write(f))
        return 0;
    return n;
}

int fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;
    return fwrite(&ch, 1, 1, f) == 1 ? ch : EOF;
}

int putc(int c, FILE *f) { return fputc(c, f); }

int fputs(const char *s, FILE *f)
{
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char small[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    if (n < (int)sizeof small) {
        fwrite(small, 1, (size_t)n, f);
    } else {
        char *big = malloc((size_t)n + 1);
        if (big) {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            fwrite(big, 1, (size_t)n, f);
            free(big);
        }
    }
    va_end(ap2);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, (size_t)-1 >> 1, fmt, ap);
    va_end(ap);
    return n;
}

int fseek(FILE *f, long off, int whence)
{
    if (whence == SEEK_CUR)
        off -= (long)(f->rlen - f->rpos) + (f->unget != EOF);
    if (f->wlen && flush_write(f))
        return -1;
    f->rpos = f->rlen = 0;
    f->unget = EOF;
    long r = lseek(f->fd, off, whence);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    f->flags &= ~F_EOF;
    return 0;
}

long ftell(FILE *f)
{
    long r = lseek(f->fd, 0, SEEK_CUR);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r - (long)(f->rlen - f->rpos) - (f->unget != EOF) + (long)f->wlen;
}

int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    (void)buf, (void)size;      // always uses the built-in buffer
    fflush(f);
    f->mode = mode;
    return 0;
}

FILE *tmpfile(void)
{
    errno = ENOSYS;
    return NULL;
}

char *tmpnam(char *s)
{
    (void)s;
    return NULL;
}

int remove(const char *path)
{
    int r = unlink(path);
    if (r == -EISDIR)
        r = rmdir(path);
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return 0;
}

void perror(const char *s)
{
    if (s && *s)
        fprintf(stderr, "%s: ", s);
    fprintf(stderr, "%s\n", strerror(errno));
}
