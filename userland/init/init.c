// init: process + filesystem self-tests, then keeps /bin/sh running on the console and reaps orphans.
#include "ulib.h"

static int failures;

static void check(const char *what, int ok)
{
    puts(ok ? "[init] PASS " : "[init] FAIL ");
    puts(what);
    puts("\n");
    if (!ok)
        failures++;
}

static int run(char *const argv[])
{
    char *env[] = { "HOME=/", "TERM=vt100", 0 };
    int pid = fork();
    if (pid == 0) {
        execve(argv[0], argv, env);
        exit(127);
    }
    return pid;
}

static void process_tests(void)
{
    int st = -1;
    check("getpid() == 1", getpid() == 1);
    char *hello[] = { "/bin/hello", "from", "init", 0 };
    int pid = run(hello);
    check("fork() returns child pid", pid > 1);
    check("waitpid() returns that pid", waitpid(pid, &st, 0) == pid);
    check("exit status 42 via WEXITSTATUS", (st >> 8) == 42 && (st & 0x7f) == 0);

    char *segv[] = { "/bin/segv", 0 };
    waitpid(run(segv), &st, 0);
    check("kernel memory access kills child with SIGSEGV", (st & 0x7f) == SIGSEGV);

    char *missing[] = { "/bin/nonexistent", 0 };
    waitpid(run(missing), &st, 0);
    check("execve of missing program fails (child exits 127)", (st >> 8) == 127);

    check("write() with kernel pointer returns -EFAULT", write(1, (const void *)0x80000, 4) == -EFAULT);
    check("write() to bad fd returns -EBADF", write(9, "x", 1) == -EBADF);
    check("waitpid() with no children returns -ECHILD", waitpid(-1, &st, 0) == -ECHILD);

    char *a[] = { "/bin/spin", "A", 0 }, *b[] = { "/bin/spin", "B", 0 };
    run(a);
    run(b);
    int reaped = 0;
    while (waitpid(-1, &st, 0) > 0)
        reaped++;
    check("two concurrent children reaped", reaped == 2);

    char *fa[] = { "/bin/fpu", "a", 0 }, *fb[] = { "/bin/fpu", "b", 0 };
    int f1 = run(fa), f2 = run(fb), s1 = -1, s2 = -1;
    waitpid(f1, &s1, 0);
    waitpid(f2, &s2, 0);
    check("FP state kept per process across preemption", s1 == 0 && s2 == 0);

    pid = run(a);
    check("kill() running child", kill(pid, SIGTERM) == 0);
    check("killed child status is SIGTERM", waitpid(pid, &st, 0) == pid && (st & 0x7f) == SIGTERM);
    check("kill() missing pid -> -ESRCH", kill(9999, SIGTERM) == -ESRCH);
    check("kill() init -> -EPERM", kill(1, SIGKILL) == -EPERM);

    // Child blocked in read() on an empty pipe is woken and killed.
    int p[2];
    pipe(p);
    pid = fork();
    if (pid == 0) {
        char c;
        close(p[1]);
        read(p[0], &c, 1);
        exit(0);
    }
    close(p[0]);
    struct procinfo pi[64];
    int np = getprocs(pi, 64), seen = 0;
    for (int i = 0; i < np; i++)
        seen |= pi[i].pid == pid && pi[i].pgid == 1;
    check("getprocs() lists child with inherited pgid", seen);
    check("setpgid(child, 0) puts it in its own group", setpgid(pid, 0) == 0);
    kill(pid, SIGKILL);
    check("blocked child killed with SIGKILL", waitpid(pid, &st, 0) == pid && (st & 0x7f) == SIGKILL);
    close(p[1]);
}

static void libc_tests(void)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%d|%5s|%-3c|%04x|%lu|%.2s|%%", -42, "ab", 'z', 0xbe, 123456789012UL, "xyz");
    check("snprintf formats", strcmp(buf, "-42|   ab|z  |00be|123456789012|xy|%") == 0);
    check("snprintf truncates, returns full length", snprintf(buf, 4, "abcdef") == 6 && strcmp(buf, "abc") == 0);
    snprintf(buf, sizeof buf, "%.3f|%g|%e|%g|%.14g|%5.1f", 3.14159, 0.0001, 12345.678, 1e20, 0.1, -2.25);
    check("snprintf floats", strcmp(buf, "3.142|0.0001|1.234568e+04|1e+20|0.1| -2.2") == 0 ||
                             strcmp(buf, "3.142|0.0001|1.234568e+04|1e+20|0.1| -2.3") == 0);

    // Lua: interpreter, float formatting, strtod, libm, stdio, error handling.
    char *lua[] = { "/bin/lua", "-e",
        "assert(2^10 == 1024 and 7 // 2 == 3 and 3 % -2 == -1)"
        "assert(tostring(1/3) == '0.33333333333333' and tostring(1e100) == '1e+100')"
        "assert(tonumber('3.25') == 3.25 and tonumber('0x10') == 16)"
        "assert(math.abs(math.sin(1) - 0.8414709848079) < 1e-12 and math.log(8, 2) == 3)"
        "local f = assert(io.open('/tmp/l.txt', 'w')) f:write('a\\nb\\n') f:close()"
        "local n = 0 for _ in io.lines('/tmp/l.txt') do n = n + 1 end assert(n == 2)"
        "assert(not pcall(error, 'x')) os.remove('/tmp/l.txt') os.exit(5)", 0 };
    int lst = 0, lpid = run(lua);
    check("lua -e script passes its asserts", waitpid(lpid, &lst, 0) == lpid && lst == W_EXITCODE(5));

    long b0 = brk(0);
    char *p[64];
    int ok = 1;
    for (int i = 0; i < 64; i++) {
        p[i] = malloc((size_t)(i + 1) * 100);
        ok &= p[i] && ((unsigned long)p[i] & 15) == 0;
        if (p[i])
            memset(p[i], i, (size_t)(i + 1) * 100);
    }
    for (int i = 0; i < 64 && ok; i++)
        ok &= p[i][0] == i && p[i][(i + 1) * 100 - 1] == i;
    check("malloc: 64 aligned blocks, contents intact", ok);
    check("heap grew via brk", brk(0) > b0);
    for (int i = 0; i < 64; i += 2)
        free(p[i]);
    char *r = realloc(p[1], 100000);
    check("realloc keeps data", r && r[0] == 1 && r[199] == 1);
    for (int i = 3; i < 64; i += 2)
        free(p[i]);
    free(r);
    long b1 = brk(0);
    void *big = malloc(200000);
    free(big);
    check("freed memory is reused (coalesced)", big && brk(0) == b1);
    check("brk below heap start fails", brk((void *)0x1000) == b1);

    struct timespec t0, t1, d = { 0, 50000000 };
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int sr = nanosleep(&d, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check("nanosleep(50 ms) sleeps 50-100 ms", sr == 0 && ms >= 50 && ms < 100);
}

static volatile int got, hits;

static void on_usr1(int sig)
{
    got = sig;
    if (++hits == 1)
        kill(getpid(), SIGUSR1);        // blocked while this handler runs: delivered after it
    __asm__ volatile("movi v16.16b, #0" ::: "v16");     // clobber FP; sigreturn must restore it
}

static void signal_tests(void)
{
    check("signal(SIGKILL) refused", (long)signal(SIGKILL, on_usr1) == -EINVAL);
    int pid = fork(), st = 0;
    if (pid == 0) {                     // init itself cannot be signalled
        signal(SIGUSR1, on_usr1);
        uint64_t lo;
        register long x0 __asm__("x0") = getpid();
        register long x1 __asm__("x1") = SIGUSR1;
        register long x8 __asm__("x8") = SYS_kill;
        __asm__ volatile("movi v16.2d, #0xff\n svc #0\n fmov %1, d16"
                         : "+r"(x0), "=r"(lo) : "r"(x1), "r"(x8) : "v16", "memory");
        exit((got == SIGUSR1 && x0 == 0) | (hits == 2) << 1 | (lo == 0xff) << 2);
    }
    waitpid(pid, &st, 0);
    st >>= 8;
    check("handler runs, kill() result intact", st & 1);
    check("same signal deferred until handler returns", st & 2);
    check("FP registers restored after handler", st & 4);

    pid = fork();
    if (pid == 0) {
        signal(SIGUSR1, on_usr1);
        struct timespec d = { 5, 0 };
        int r = nanosleep(&d, 0);
        exit(got == SIGUSR1 && r == -EINTR ? 0 : 1);
    }
    struct timespec d = { 0, 50000000 };
    nanosleep(&d, 0);
    kill(pid, SIGUSR1);
    check("caught signal interrupts nanosleep with -EINTR", waitpid(pid, &st, 0) == pid && st == 0);

    signal(SIGTERM, (sighandler_t)SIG_IGN);
    pid = fork();
    if (pid == 0) {
        struct timespec d = { 0, 100000000 };
        nanosleep(&d, 0);
        exit(7);
    }
    signal(SIGTERM, (sighandler_t)SIG_DFL);
    kill(pid, SIGTERM);
    check("SIG_IGN inherited across fork; SIGTERM ignored", waitpid(pid, &st, 0) == pid && st == W_EXITCODE(7));
}

static void fs_tests(void)
{
    char buf[128];
    struct stat st;

    int fd = open("/tmp/a.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check("open(O_CREAT) new file", fd >= 3);
    check("write() 11 bytes", write(fd, "hello world", 11) == 11);
    check("lseek(SEEK_SET 6)", lseek(fd, 6, SEEK_SET) == 6);
    check("overwrite at offset", write(fd, "WORLD!", 6) == 6);
    close(fd);

    fd = open("/tmp/a.txt", O_RDONLY);
    long n = read(fd, buf, sizeof buf);
    buf[n > 0 ? n : 0] = 0;
    check("read back \"hello WORLD!\"", n == 12 && strcmp(buf, "hello WORLD!") == 0);
    check("read at EOF returns 0", read(fd, buf, sizeof buf) == 0);
    check("write to O_RDONLY fd -> -EBADF", write(fd, "x", 1) == -EBADF);
    check("fstat size 12, regular", fstat(fd, &st) == 0 && st.st_size == 12 && S_ISREG(st.st_mode));
    close(fd);
    check("read on closed fd -> -EBADF", read(fd, buf, 1) == -EBADF);

    fd = open("/tmp/a.txt", O_WRONLY | O_APPEND);
    write(fd, "++", 2);
    close(fd);
    check("O_APPEND grows file to 14", stat("/tmp/a.txt", &st) == 0 && st.st_size == 14);
    check("O_CREAT|O_EXCL on existing -> -EEXIST", open("/tmp/a.txt", O_WRONLY | O_CREAT | O_EXCL, 0644) == -EEXIST);
    check("open missing -> -ENOENT", open("/tmp/nope", O_RDONLY) == -ENOENT);
    check("open dir for write -> -EISDIR", open("/tmp", O_WRONLY) == -EISDIR);

    check("mkdir /tmp/d", mkdir("/tmp/d", 0755) == 0);
    check("mkdir again -> -EEXIST", mkdir("/tmp/d", 0755) == -EEXIST);
    check("chdir /tmp/d", chdir("/tmp/d") == 0);
    check("getcwd == /tmp/d", getcwd(buf, sizeof buf) == 7 && strcmp(buf, "/tmp/d") == 0);
    check("relative create ../d/b.txt", (fd = open("../d/b.txt", O_RDWR | O_CREAT, 0644)) >= 0);
    close(fd);
    check("rename b.txt -> c.txt", rename("b.txt", "c.txt") == 0 && stat("/tmp/d/c.txt", &st) == 0
          && stat("/tmp/d/b.txt", &st) == -ENOENT);
    check("rename dir into itself -> -EINVAL", rename("/tmp/d", "/tmp/d/x") == -EINVAL);
    check("rmdir non-empty -> -ENOTEMPTY", rmdir("/tmp/d") == -ENOTEMPTY);
    check("unlink dir -> -EISDIR", unlink("/tmp/d") == -EISDIR);
    check("unlink c.txt", unlink("c.txt") == 0);
    chdir("/");
    check("rmdir /tmp/d", rmdir("/tmp/d") == 0);
    check("getcwd == /", getcwd(buf, sizeof buf) == 2 && strcmp(buf, "/") == 0);

    // Unlinked-but-open file stays readable until closed.
    fd = open("/tmp/a.txt", O_RDONLY);
    unlink("/tmp/a.txt");
    check("unlinked open file still readable", read(fd, buf, 5) == 5 && stat("/tmp/a.txt", &st) == -ENOENT);
    close(fd);

    // Directory listing of / must contain the standard tree.
    fd = open("/", O_RDONLY);
    struct dirent de[16];
    long nb = getdents(fd, de, sizeof de);
    int found = 0;
    for (long i = 0; i < nb / (long)sizeof *de; i++)
        for (const char *const *w = (const char *const[]){ "bin", "dev", "etc", "tmp", ".", "..", 0 }; *w; w++)
            found += strcmp(de[i].d_name, *w) == 0;
    check("getdents(/) lists . .. bin dev etc tmp", found == 6);
    close(fd);

    check("stat /dev/console is char device", stat("/dev/console", &st) == 0 && (st.st_mode & S_IFMT) == S_IFCHR);
    fd = open("/dev/null", O_RDWR);
    check("/dev/null: write all, read EOF", write(fd, "abc", 3) == 3 && read(fd, buf, 3) == 0);
    close(fd);

    // Pipe + dup2: child writes to the pipe via its stdout.
    int p[2];
    check("pipe()", pipe(p) == 0);
    int pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        char *av[] = { "/bin/hello", "piped", 0 };
        execve(av[0], av, 0);
        exit(127);
    }
    close(p[1]);
    size_t tot = 0;
    while ((n = read(p[0], buf + tot, sizeof buf - 1 - tot)) > 0)
        tot += (size_t)n;
    buf[tot] = 0;
    close(p[0]);
    int st2;
    waitpid(pid, &st2, 0);
    const char *want = "argc 2: /bin/hello piped\n";
    size_t wl = strlen(want);
    puts("[init] pipe got: ");
    puts(buf);
    check("exec'd child output arrives through pipe, then EOF",
          tot > wl && strcmp(buf + tot - wl, want) == 0 && (st2 >> 8) == 42);
    check("write to pipe with no reader -> -EPIPE", pipe(p) == 0 && close(p[0]) == 0 && write(p[1], "x", 1) == -EPIPE);
    close(p[1]);
    check("dup() returns lowest free fd", (fd = dup(0)) == 3);
    close(fd);
}

int main(void)
{
    puts("[init] running as pid ");
    putnum(getpid());
    puts("\n");
    process_tests();
    libc_tests();
    signal_tests();
    fs_tests();
    puts(failures ? "[init] SOME TESTS FAILED\n" : "[init] all tests passed\n");

    char *sh[] = { "/bin/sh", 0 };
    for (;;) {
        puts("[init] starting /bin/sh (^D or exit to restart it)\n");
        int shpid = run(sh), st;
        if (shpid < 0)
            return 1;
        while (waitpid(-1, &st, 0) != shpid)   // also reaps reparented orphans
            ;
    }
}
