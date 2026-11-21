// Full-screen text editor for a VT100/ANSI serial terminal.
//   arrows move, Home/End (or ^A/^E) line start/end, PgUp/PgDn scroll a page
//   Enter splits the line, Backspace joins, ^S saves, ^Q quits (twice if unsaved)
// shortcut: assumes an 80x24 terminal and no tabs/UTF-8 width handling; query
// the size with "\e[6n" if other terminals matter.
#include "ulib.h"

#define COLS 80
#define ROWS 23                 // text rows; the last row is the status line

struct line {
    char *s;
    long len, cap;
};

static struct line *L;
static long nl, lcap;
static long cy, cx, top, left;  // cursor (line, column) and viewport origin
static int dirty, quit_armed;
static char fname[PATH_MAX], msg[COLS];

static char out[4096];
static size_t outn;

static void flush(void)
{
    write(1, out, outn);
    outn = 0;
}

static void emit(const char *s, size_t n)
{
    while (n) {
        size_t k = sizeof out - outn < n ? sizeof out - outn : n;
        memcpy(out + outn, s, k);
        outn += k, s += k, n -= k;
        if (outn == sizeof out)
            flush();
    }
}

static void emits(const char *s) { emit(s, strlen(s)); }

static void die(const char *what)
{
    ttymode(TTY_COOKED);
    emits("\033[2J\033[H");
    flush();
    eputs("edit: ");
    eputs(what);
    eputs("\n");
    exit(1);
}

static void reserve(struct line *l, long need)
{
    if (need <= l->cap)
        return;
    long c = l->cap ? l->cap : 16;
    while (c < need)
        c *= 2;
    char *s = realloc(l->s, (size_t)c);
    if (!s)
        die("out of memory");
    l->s = s, l->cap = c;
}

// Insert an empty line at index at.
static struct line *line_insert(long at)
{
    if (nl == lcap) {
        long c = lcap ? lcap * 2 : 64;
        struct line *n = realloc(L, (size_t)c * sizeof *n);
        if (!n)
            die("out of memory");
        L = n, lcap = c;
    }
    memmove(L + at + 1, L + at, (size_t)(nl - at) * sizeof *L);
    nl++;
    L[at] = (struct line){ 0 };
    reserve(&L[at], 1);
    return &L[at];
}

static void line_append(struct line *l, const char *s, long n)
{
    reserve(l, l->len + n);
    memcpy(l->s + l->len, s, (size_t)n);
    l->len += n;
}

static void load(void)
{
    int fd = open(fname, O_RDONLY);
    struct line *cur = line_insert(0);
    if (fd < 0) {
        snprintf(msg, sizeof msg, "%s", fd == -ENOENT ? "new file" : "cannot read file");
        return;
    }
    char buf[1024];
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        for (long i = 0, start = 0; i <= n; i++)
            if (i == n || buf[i] == '\n') {
                line_append(cur, buf + start, i - start);
                if (i < n)
                    cur = line_insert(nl);
                start = i + 1;
            }
    close(fd);
    if (nl > 1 && cur->len == 0) {      // trailing newline made an empty last line
        free(cur->s);
        nl--;
    }
    snprintf(msg, sizeof msg, "%ld lines", nl);
}

static void save(void)
{
    int fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(msg, sizeof msg, "cannot write %s (error %d)", fname, -fd);
        return;
    }
    long bytes = 0;
    for (long i = 0; i < nl; i++) {
        if (write(fd, L[i].s, (size_t)L[i].len) != L[i].len || write(fd, "\n", 1) != 1) {
            close(fd);
            snprintf(msg, sizeof msg, "write failed: file is incomplete");
            return;
        }
        bytes += L[i].len + 1;
    }
    close(fd);
    dirty = 0;
    snprintf(msg, sizeof msg, "wrote %ld bytes", bytes);
}

static void draw(void)
{
    if (cy < top)
        top = cy;
    if (cy >= top + ROWS)
        top = cy - ROWS + 1;
    if (cx < left)
        left = cx;
    if (cx >= left + COLS)
        left = cx - COLS + 1;
    char tmp[COLS + 32];
    emits("\033[?25l\033[H");           // hide cursor while repainting
    for (long r = 0; r < ROWS; r++) {
        long i = top + r;
        if (i < nl) {
            long n = L[i].len - left;
            if (n > COLS)
                n = COLS;
            if (n > 0)
                emit(L[i].s + left, (size_t)n);
        } else {
            emits("~");
        }
        emits("\033[K\r\n");
    }
    int n = snprintf(tmp, sizeof tmp, "\033[7m %.40s%s  line %ld/%ld col %ld  %s",
                     fname, dirty ? " [modified]" : "", cy + 1, nl, cx + 1, msg);
    emit(tmp, (size_t)n);
    emits("\033[K\033[0m");
    n = snprintf(tmp, sizeof tmp, "\033[%ld;%ldH\033[?25h", cy - top + 1, cx - left + 1);
    emit(tmp, (size_t)n);
    flush();
}

static int key(void)
{
    char c;
    if (read(0, &c, 1) != 1)
        return -1;
    if (c != 033)
        return (unsigned char)c;
    char s[3];
    if (read(0, s, 1) != 1 || (s[0] != '[' && s[0] != 'O') || read(0, s + 1, 1) != 1)
        return 033;
    switch (s[1]) {
    case 'A': return 0x100;     // up
    case 'B': return 0x101;     // down
    case 'C': return 0x102;     // right
    case 'D': return 0x103;     // left
    case 'H': return 1;         // Home = ^A
    case 'F': return 5;         // End = ^E
    }
    if (s[1] >= '0' && s[1] <= '9') {   // \e[N~
        char t;
        if (read(0, &t, 1) != 1 || t != '~')
            return 033;
        switch (s[1]) {
        case '1': case '7': return 1;
        case '4': case '8': return 5;
        case '3': return 0x104;         // Delete
        case '5': return 0x105;         // PgUp
        case '6': return 0x106;         // PgDn
        }
    }
    return 033;
}

static void edit_key(int k)
{
    struct line *l = &L[cy];
    switch (k) {
    case 0x100: if (cy > 0) cy--; break;
    case 0x101: if (cy < nl - 1) cy++; break;
    case 0x103:
        if (cx > 0)
            cx--;
        else if (cy > 0)
            cy--, cx = L[cy].len;
        break;
    case 0x102:
        if (cx < l->len)
            cx++;
        else if (cy < nl - 1)
            cy++, cx = 0;
        break;
    case 0x105: cy = cy > ROWS ? cy - ROWS : 0; break;
    case 0x106: cy = cy + ROWS < nl ? cy + ROWS : nl - 1; break;
    case 1: cx = 0; break;
    case 5: cx = l->len; break;
    case 19: save(); break;
    case '\r': case '\n': {
        long tail = l->len - cx;
        struct line *n = line_insert(cy + 1);
        l = &L[cy];                     // line_insert may move L
        line_append(n, l->s + cx, tail);
        l->len = cx;
        cy++, cx = 0, dirty = 1;
        break;
    }
    case 0x7f: case '\b':
        if (cx > 0) {
            memmove(l->s + cx - 1, l->s + cx, (size_t)(l->len - cx));
            l->len--, cx--, dirty = 1;
        } else if (cy > 0) {
            struct line *p = &L[cy - 1];
            cx = p->len;
            line_append(p, l->s, l->len);
            free(l->s);
            memmove(L + cy, L + cy + 1, (size_t)(nl - cy - 1) * sizeof *L);
            nl--, cy--, dirty = 1;
        }
        break;
    case 0x104:
        if (cx < l->len) {
            memmove(l->s + cx, l->s + cx + 1, (size_t)(l->len - cx - 1));
            l->len--, dirty = 1;
        } else if (cy < nl - 1) {
            line_append(l, L[cy + 1].s, L[cy + 1].len);
            free(L[cy + 1].s);
            memmove(L + cy + 1, L + cy + 2, (size_t)(nl - cy - 2) * sizeof *L);
            nl--, dirty = 1;
        }
        break;
    default:
        if (k >= 0x20 && k < 0x7f) {
            reserve(l, l->len + 1);
            memmove(l->s + cx + 1, l->s + cx, (size_t)(l->len - cx));
            l->s[cx++] = (char)k;
            l->len++, dirty = 1;
        }
    }
    if (cx > L[cy].len)
        cx = L[cy].len;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        eputs("usage: edit FILE\n");
        return 1;
    }
    snprintf(fname, sizeof fname, "%s", argv[1]);
    load();
    ttymode(TTY_RAW);
    emits("\033[2J");
    for (;;) {
        draw();
        int k = key();
        msg[0] = 0;
        if (k < 0)
            die("input closed");
        if (k == 17) {                  // ^Q
            if (dirty && !quit_armed) {
                quit_armed = 1;
                snprintf(msg, sizeof msg, "unsaved changes: ^S to save, ^Q again to discard");
                continue;
            }
            break;
        }
        quit_armed = 0;
        edit_key(k);
    }
    ttymode(TTY_COOKED);
    emits("\033[2J\033[H");
    flush();
    return 0;
}
