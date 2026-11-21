// Phase 5 init: process + filesystem self-tests, then a console echo loop.
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
    fs_tests();
    puts(failures ? "[init] SOME TESTS FAILED\n" : "[init] all tests passed\n");

    puts("[init] console echo: type lines, ^D to finish\n");
    char line[128];
    long n;
    while (puts("> "), (n = read(0, line, sizeof line)) > 0) {
        puts("[init] got: ");
        write(1, line, (size_t)n);
    }
    puts("[init] EOF, exiting\n");
    return failures;
}
