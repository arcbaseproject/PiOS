// Minimal shell: pipelines, < > >> redirection, '' "" \ quoting, cd/exit builtins.
// On the console: line editing (arrows, Home/End, Delete) and up/down history.
// Commands without '/' are searched in $PATH (default /bin:/boot/bin). Each
// pipeline gets its own process group and owns the console's ^C while it runs.
#include "ulib.h"

#define MAX_ARGS 16
#define MAX_CMDS 8

struct cmd {
    char *argv[MAX_ARGS + 1];
    int argc;
    char *in, *out;
    int append;
};

static char line[256], words[512];
static struct cmd cmds[MAX_CMDS];

// Operator tokens are returned as these static strings, so `>` quoted as a
// word compares unequal by pointer.
static char OP_PIPE[] = "|", OP_IN[] = "<", OP_OUT[] = ">", OP_APP[] = ">>";

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n'; }

// Split line into toks; returns count or -1 on error.
static int tokenize(char **toks, int max)
{
    char *s = line, *w = words;
    int n = 0;
    for (;;) {
        while (is_space(*s))
            s++;
        if (!*s)
            return n;
        if (n == max)
            return -1;
        if (*s == '|') {
            toks[n++] = OP_PIPE, s++;
        } else if (*s == '<') {
            toks[n++] = OP_IN, s++;
        } else if (*s == '>') {
            toks[n++] = s[1] == '>' ? (s += 2, OP_APP) : (s++, OP_OUT);
        } else {
            toks[n++] = w;
            while (*s && !is_space(*s) && *s != '|' && *s != '<' && *s != '>') {
                if (*s == '\'' || *s == '"') {
                    char q = *s++;
                    while (*s && *s != q) {
                        if (q == '"' && *s == '\\' && s[1] && strchr("\"\\$`", s[1]))
                            s++;        // \" \\ \$ \` inside double quotes (POSIX)
                        *w++ = *s++;
                    }
                    if (!*s)
                        return -1;  // unterminated quote
                    s++;
                } else {
                    if (*s == '\\' && s[1])
                        s++;            // backslash quotes the next character
                    *w++ = *s++;
                }
            }
            *w++ = 0;
        }
    }
}

// Group tokens into commands; returns command count, 0 for blank, -1 on error.
static int parse(void)
{
    char *toks[64];
    int nt = tokenize(toks, 64);
    if (nt <= 0)
        return nt;
    memset(cmds, 0, sizeof cmds);
    int nc = 0;
    struct cmd *c = &cmds[0];
    for (int i = 0; i < nt; i++) {
        char *t = toks[i];
        if (t == OP_PIPE) {
            if (!c->argc || ++nc == MAX_CMDS)
                return -1;
            c = &cmds[nc];
        } else if (t == OP_IN || t == OP_OUT || t == OP_APP) {
            if (i + 1 == nt || toks[i + 1] == OP_PIPE || toks[i + 1] == OP_IN ||
                toks[i + 1] == OP_OUT || toks[i + 1] == OP_APP)
                return -1;
            if (t == OP_IN)
                c->in = toks[++i];
            else
                c->out = toks[++i], c->append = t == OP_APP;
        } else {
            if (c->argc == MAX_ARGS)
                return -1;
            c->argv[c->argc++] = t;
        }
    }
    return c->argc ? nc + 1 : -1;
}

static _Noreturn void child(struct cmd *c, char **envp)
{
    if (c->in) {
        int fd = open(c->in, O_RDONLY);
        if (fd < 0) {
            err("sh", c->in, fd);
            exit(1);
        }
        dup2(fd, 0);
        close(fd);
    }
    if (c->out) {
        int fd = open(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) {
            err("sh", c->out, fd);
            exit(1);
        }
        dup2(fd, 1);
        close(fd);
    }
    const char *name = c->argv[0];
    int slash = 0;
    for (const char *p = name; *p; p++)
        slash |= *p == '/';
    int r = -ENOENT;
    if (slash) {
        r = execve(name, c->argv, envp);
    } else {
        // $PATH, e.g. /bin (built-in programs, RAM), /usr/bin (BusyBox applets),
        // /boot/bin (installed on the SD card).
        const char *dirs = "/bin:/boot/bin";
        for (char **e = envp; e && *e; e++)
            if (strncmp(*e, "PATH=", 5) == 0)
                dirs = *e + 5;
        size_t nl = strlen(name);
        while (*dirs && (r == -ENOENT || r == -ENOTDIR)) {
            const char *end = strchr(dirs, ':');
            size_t dl = end ? (size_t)(end - dirs) : strlen(dirs);
            char path[PATH_MAX];
            if (dl + 1 + nl < sizeof path) {
                memcpy(path, dirs, dl);
                path[dl] = '/';
                memcpy(path + dl + 1, name, nl + 1);
                r = execve(path, c->argv, envp);
            }
            dirs += dl + (end != NULL);
        }
    }
    err("sh", name, r);
    exit(127);
}

static void run(int n, char **envp)
{
    int pids[MAX_CMDS], pgid = 0, prev = -1;
    for (int i = 0; i < n; i++) {
        int p[2] = { -1, -1 };
        if (i < n - 1 && pipe(p) < 0) {
            eputs("sh: pipe failed\n");
            n = i;
            break;
        }
        int pid = fork();
        if (pid == 0) {
            setpgid(0, pgid);
            if (prev >= 0) {
                dup2(prev, 0);
                close(prev);
            }
            if (p[1] >= 0) {
                dup2(p[1], 1);
                close(p[0]);
                close(p[1]);
            }
            child(&cmds[i], envp);
        }
        if (prev >= 0)
            close(prev);
        if (p[1] >= 0)
            close(p[1]);
        prev = p[0];
        if (pid < 0) {
            eputs("sh: fork failed\n");
            n = i;
            break;
        }
        if (!pgid)
            pgid = pid;
        setpgid(pid, pgid);     // also in child: whichever runs first wins the race
        pids[i] = pid;
    }
    if (prev >= 0)
        close(prev);
    tcsetpgrp(pgid);
    for (int i = 0; i < n; i++) {
        int st = 0;
        waitpid(pids[i], &st, 0);
        int sig = st & 0x7f;
        if (sig && sig != SIGINT) {
            eputs("sh: ");
            eputs(cmds[i].argv[0]);
            eputs(sig == SIGSEGV ? ": segmentation fault\n" : ": killed by signal\n");
        }
    }
    tcsetpgrp(0);
    ttymode(TTY_COOKED);    // in case a raw-mode program died without restoring it
}

#define HIST 16
static char hist[HIST][sizeof line];
static int nhist;           // total lines ever added; slot = index % HIST

static void outn(const char *s, long n)
{
    if (n > 0)
        write(1, s, (size_t)n);
}

static void left(long n)
{
    char b[16];
    if (n > 0)
        outn(b, snprintf(b, sizeof b, "\033[%ldD", n));
}

// Replace the edited text (cursor at pos) with s.
static void replace(const char *s, long *len, long *pos)
{
    left(*pos);
    *len = *pos = (long)strlen(s);
    memcpy(line, s, (size_t)*len);
    outn(line, *len);
    outn("\033[K", 3);
}

// Read one line into `line` with raw-mode editing. Returns length, -1 at EOF.
static long edit_line(void)
{
    long len = 0, pos = 0;
    int h = nhist;          // history cursor; nhist = the line being typed
    char saved[sizeof line] = "";
    ttymode(TTY_RAW);
    for (;;) {
        char c, s[3];
        if (read(0, &c, 1) != 1) {
            len = -1;
            break;
        }
        if (c == '\r' || c == '\n') {
            outn("\n", 1);
            break;
        }
        if (c == 3) {               // ^C: drop the line
            outn("^C\n", 3);
            len = 0;
            break;
        }
        if (c == 4 && len == 0) {   // ^D on an empty line: EOF
            len = -1;
            break;
        }
        int k = c;
        if (c == 033) {
            if (read(0, s, 1) != 1 || read(0, s + 1, 1) != 1)
                continue;
            k = s[1] == '3' ? (read(0, s, 1), 0x104) : 0x100 + s[1];
        }
        if (k == 0x100 + 'D' && pos > 0) {
            left(1), pos--;
        } else if (k == 0x100 + 'C' && pos < len) {
            outn(line + pos, 1), pos++;
        } else if (k == 0x100 + 'H' || k == 1) {
            left(pos), pos = 0;
        } else if (k == 0x100 + 'F' || k == 5) {
            outn(line + pos, len - pos), pos = len;
        } else if ((k == 0x100 + 'A' && h > 0 && h > nhist - HIST) || (k == 0x100 + 'B' && h < nhist)) {
            if (h == nhist) {
                memcpy(saved, line, (size_t)len);
                saved[len] = 0;
            }
            h += k == 0x100 + 'A' ? -1 : 1;
            replace(h == nhist ? saved : hist[h % HIST], &len, &pos);
        } else if ((k == 0x7f || k == '\b' || k == 0x104) && (k == 0x104 ? pos < len : pos > 0)) {
            if (k != 0x104)
                left(1), pos--;
            memmove(line + pos, line + pos + 1, (size_t)(len - pos - 1));
            len--;
            outn(line + pos, len - pos);
            outn(" ", 1);
            left(len - pos + 1);
        } else if (k >= 0x20 && k < 0x7f && len < (long)sizeof line - 1) {
            memmove(line + pos + 1, line + pos, (size_t)(len - pos));
            line[pos] = (char)k;
            len++;
            outn(line + pos, len - pos);
            pos++;
            left(len - pos);
        }
    }
    ttymode(TTY_COOKED);
    if (len > 0) {
        line[len] = 0;
        if (!nhist || strcmp(hist[(nhist - 1) % HIST], line) != 0)
            memcpy(hist[nhist++ % HIST], line, (size_t)len + 1);
    }
    return len;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc, (void)argv;
    struct stat st;
    int console = fstat(0, &st) == 0 && (st.st_mode & S_IFMT) == S_IFCHR;
    for (;;) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd) < 0)
            cwd[0] = '?', cwd[1] = 0;
        puts(cwd);
        puts(" $ ");
        // shortcut: non-console stdin assumes one read() = one line; a pipe/file stdin needs buffering.
        long len = console ? edit_line() : read(0, line, sizeof line - 1);
        if (len < 0 || (len == 0 && !console)) {     // console blank line / ^C = 0, not EOF
            puts("\n");
            return 0;
        }
        line[len] = 0;
        int n = parse();
        if (n < 0) {
            eputs("sh: syntax error\n");
            continue;
        }
        if (n == 0)
            continue;
        char **av = cmds[0].argv;
        if (strcmp(av[0], "exit") == 0)
            return 0;
        if (strcmp(av[0], "cd") == 0) {
            int r = chdir(av[1] ? av[1] : "/");
            if (r < 0)
                err("cd", av[1], r);
            continue;
        }
        run(n, envp);
    }
}
