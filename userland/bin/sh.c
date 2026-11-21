// Minimal shell: pipelines, < > >> redirection, '' "" quoting, cd/exit builtins.
// Commands without '/' are run from /bin, then /boot/bin (SD card). Each
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
                    while (*s && *s != q)
                        *w++ = *s++;
                    if (!*s)
                        return -1;  // unterminated quote
                    s++;
                } else {
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
    } else if (strlen(name) < PATH_MAX - 16) {
        // /bin = built-in programs (RAM); /boot/bin = installed on the SD card.
        static const char *const dirs[] = { "/bin/", "/boot/bin/" };
        for (int i = 0; i < 2 && r == -ENOENT; i++) {
            char path[PATH_MAX];
            size_t dl = strlen(dirs[i]);
            memcpy(path, dirs[i], dl);
            memcpy(path + dl, name, strlen(name) + 1);
            r = execve(path, c->argv, envp);
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

int main(int argc, char **argv, char **envp)
{
    (void)argc, (void)argv;
    for (;;) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd) < 0)
            cwd[0] = '?', cwd[1] = 0;
        puts(cwd);
        puts(" $ ");
        // shortcut: assumes one read() = one line (true for the console); a pipe/file stdin needs buffering.
        long len = read(0, line, sizeof line - 1);
        if (len <= 0) {
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
