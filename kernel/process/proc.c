// Process lifecycle: init creation, fork, execve, exit, waitpid.
#include <stddef.h>
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "sched.h"
#include "uapi.h"
#include "timer.h"
#include "vfs.h"

#define MAX_ARGS 32

extern void ret_from_fork(void);
static struct task *init_task;

static struct trap_frame *task_frame(struct task *t)
{
    return (struct trap_frame *)((uintptr_t)t->kstack + PAGE_SIZE - sizeof(struct trap_frame));
}

// Copy a NUL-terminated user string. Returns length or -errno.
long copyin_str(char *dst, uint64_t uva, size_t max)
{
    uint64_t *pgd = current_task()->pgd;
    for (size_t i = 0; i < max; i++) {
        uint64_t va = uva + i;
        if ((i == 0 || (va & (PAGE_SIZE - 1)) == 0) && !vm_check(pgd, va, 1, 0))
            return -EFAULT;
        if (!(dst[i] = *(const char *)va))
            return (long)i;
    }
    return -E2BIG;
}

// Append a NULL-terminated user string vector to buf. Returns count or -errno.
static long copyin_strv(uint64_t uvec, char *buf, size_t *used, size_t cap)
{
    if (!uvec)
        return 0;
    for (long n = 0;; n++) {
        if (n >= MAX_ARGS)
            return -E2BIG;
        uint64_t slot = uvec + (uint64_t)n * 8;
        if (!vm_check(current_task()->pgd, slot, 8, 0))
            return -EFAULT;
        uint64_t p = *(const uint64_t *)slot;
        if (!p)
            return n;
        long l = copyin_str(buf + *used, p, cap - *used);
        if (l < 0)
            return l;
        *used += (size_t)l + 1;
    }
}

// Build a fresh address space for initrd image `path`, with argv/envp strings
// packed in kbuf (argc strings then envc strings). On success, replaces t's
// address space and points tf at the new program's entry.
static long exec_image(struct task *t, struct trap_frame *tf, const char *path,
                       const char *kbuf, size_t used, long argc, long envc)
{
    struct inode *ino;
    long err = vfs_lookup(path, &ino);
    if (err < 0)
        return err;
    if (!S_ISREG(ino->mode)) {
        iput(ino);
        return -EACCES;
    }

    // Linux initial stack, all in the top stack page: from sp up, argc, argv[],
    // NULL, envp[], NULL, auxv pairs, AT_NULL; then 16 AT_RANDOM bytes and the
    // strings at the very top.
    enum { NAUX = 10 };
    uint64_t str_va = USER_STACK_TOP - used;
    uint64_t rnd_va = (str_va - 16) & ~15UL;
    uint64_t words = 1 + (uint64_t)(argc + 1) + (uint64_t)(envc + 1) + 2 * NAUX;
    uint64_t sp = (rnd_va - words * 8) & ~15UL;
    uint64_t *pgd = sp < USER_STACK_TOP - PAGE_SIZE ? NULL : vm_new();
    if (!pgd) {
        iput(ino);
        return sp < USER_STACK_TOP - PAGE_SIZE ? -E2BIG : -ENOMEM;
    }
    struct elf_info ei;
    err = elf_load(pgd, ino, &ei);
    iput(ino);
    uint8_t *top = NULL;
    for (uint64_t va = USER_STACK_BASE; err == 0 && va < USER_STACK_TOP; va += PAGE_SIZE)
        if (!(top = vm_alloc_page(pgd, va, VM_NX)))
            err = -ENOMEM;
    if (err) {
        vm_free(pgd);
        return err;
    }

    // top = kernel alias of the highest stack page; user va -> kernel pointer.
#define STACK_K(va) (top + ((va) - (USER_STACK_TOP - PAGE_SIZE)))
    memcpy(STACK_K(str_va), kbuf, used);
    uint64_t *rnd = (uint64_t *)STACK_K(rnd_va);
    uint64_t *w = (uint64_t *)STACK_K(sp);
#undef STACK_K
    // shortcut: AT_RANDOM from the cycle counter, not a CSPRNG; musl only seeds its stack canary.
    uint64_t c = read_sysreg(cntpct_el0);
    rnd[0] = c * 0x9E3779B97F4A7C15ul;
    rnd[1] = (c ^ (uint64_t)t->pid << 32) * 0xC2B2AE3D27D4EB4Ful;
    *w++ = (uint64_t)argc;
    uint64_t s = str_va;
    for (long i = 0; i < argc + envc; i++) {
        if (i == argc)
            *w++ = 0;       // argv terminator
        *w++ = s;
        s += strlen(kbuf + (s - str_va)) + 1;
    }
    if (envc == 0)
        *w++ = 0;           // argv terminator (loop never reached i == argc)
    *w++ = 0;               // envp terminator
    const uint64_t aux[NAUX][2] = {
        { 3, ei.phdr }, { 4, 56 }, { 5, ei.phnum },     // AT_PHDR, AT_PHENT, AT_PHNUM
        { 6, PAGE_SIZE }, { 9, ei.entry },              // AT_PAGESZ, AT_ENTRY
        { 11, 0 }, { 12, 0 }, { 16, 3 },                // AT_UID, AT_EUID, AT_HWCAP (fp, asimd)
        { 25, rnd_va }, { 0, 0 },                       // AT_RANDOM, AT_NULL
    };
    memcpy(w, aux, sizeof aux);

    uint64_t *old = t->pgd;
    t->pgd = pgd;
    if (t == current_task())
        vm_activate(pgd);
    if (old)
        vm_free(old);
    const char *base = path;
    for (const char *c = path; *c; c++)
        if (*c == '/')
            base = c + 1;
    set_task_name(t, base);

    t->brk_base = t->brk = t->brk_mapped = ei.top;
    t->mmap_low = USER_STACK_BASE - PAGE_SIZE;     // one guard page below the stack
    for (int i = 0; i < 32; i++) {      // handlers point into the old image; ignores survive (POSIX)
        if (t->sighand[i] != SIG_IGN)
            t->sighand[i] = SIG_DFL;
        t->sigflags[i] = t->sigactmask[i] = 0;
    }
    t->sigpend = 0;
    t->alarm_at = t->alarm_every = 0;
    for (int i = 0; i < NFD; i++)
        if (t->fd[i] && t->cloexec[i]) {
            file_put(t->fd[i]);
            t->fd[i] = NULL;
        }
    memset(t->cloexec, 0, sizeof t->cloexec);
    memset(&t->fp, 0, sizeof t->fp);
    t->tpidr = 0;
    if (t == current_task()) {
        fpsimd_load(&t->fp);
        write_sysreg(tpidr_el0, 0);
    }
    memset(tf, 0, sizeof *tf);
    tf->elr = ei.entry;
    tf->spsr = 0;           // EL0t, interrupts unmasked
    tf->sp_el0 = sp;
    return 0;               // dispatcher stores this in x0 (Linux: 0 at entry)
}

long sys_execve(struct trap_frame *tf, uint64_t upath, uint64_t uargv, uint64_t uenvp)
{
    char path[PATH_MAX];
    long r = copyin_str(path, upath, sizeof path);
    if (r < 0)
        return r == -E2BIG ? -ENAMETOOLONG : r;
    char *kbuf = page_alloc();
    if (!kbuf)
        return -ENOMEM;
    size_t used = 0, cap = PAGE_SIZE - (MAX_ARGS * 2 + 2) * 8 - 16;
    long argc = copyin_strv(uargv, kbuf, &used, cap);
    long envc = argc < 0 ? 0 : copyin_strv(uenvp, kbuf, &used, cap);
    if (argc < 0)
        r = argc;
    else if (envc < 0)
        r = envc;
    else
        r = exec_image(current_task(), tf, path, kbuf, used, argc, envc);
    page_free(kbuf);
    return r;
}

// Called before interrupts are enabled.
void proc_create_init(void)
{
    struct task *t = task_alloc("init");
    if (!t)
        panic("init: no task slot");
    struct trap_frame *tf = task_frame(t);
    long r = exec_image(t, tf, "/sbin/init", "init", 5, 1, 0);
    if (r < 0)
        panic("init: exec /sbin/init failed (%ld)", r);
    // fd 0/1/2 share one open file on /dev/console.
    struct inode *con;
    struct file *f = NULL;
    if (vfs_lookup("/dev/console", &con) == 0) {
        f = file_new(con, O_RDWR);
        iput(con);
    }
    if (!f)
        panic("init: cannot open /dev/console");
    t->fd[0] = t->fd[1] = t->fd[2] = f;
    f->refs = 3;
    t->ctx.lr = (uintptr_t)ret_from_fork;
    t->ctx.sp = (uintptr_t)tf;
    t->pgid = t->pid;
    t->umask = 022;
    t->state = TASK_RUNNABLE;
    init_task = t;
    kprintf("[PROC] init created, pid %d\n", t->pid);
}

long sys_fork(struct trap_frame *tf)
{
    struct task *p = current_task();
    struct task *c = task_alloc(p->name);
    if (!c)
        return -EAGAIN;
    if (!(c->pgd = vm_copy(p->pgd))) {
        task_release(c);
        return -ENOMEM;
    }
    struct trap_frame *ctf = task_frame(c);
    *ctf = *tf;
    ctf->x[0] = 0;          // child's fork() result
    fpsimd_save(&c->fp);    // parent's live FP registers
    c->ctx.lr = (uintptr_t)ret_from_fork;
    c->ctx.sp = (uintptr_t)ctf;
    c->parent = p;
    c->pgid = p->pgid;
    c->sigmask = p->sigmask, c->sigrestorer = p->sigrestorer;
    memcpy(c->sighand, p->sighand, sizeof c->sighand);
    memcpy(c->sigflags, p->sigflags, sizeof c->sigflags);
    memcpy(c->sigactmask, p->sigactmask, sizeof c->sigactmask);
    c->tpidr = read_sysreg(tpidr_el0);     // parent's live thread pointer
    c->brk_base = p->brk_base, c->brk = p->brk, c->brk_mapped = p->brk_mapped;
    c->mmap_low = p->mmap_low;
    c->umask = p->umask;
    memcpy(c->cloexec, p->cloexec, sizeof c->cloexec);
    for (int i = 0; i < NFD; i++)
        if ((c->fd[i] = p->fd[i]))
            c->fd[i]->refs++;
    if ((c->cwd = p->cwd))
        iget(c->cwd);
    c->state = TASK_RUNNABLE;
    return c->pid;
}

static void signal_task(struct task *t, int sig);

_Noreturn void do_exit(int status)
{
    irq_disable();
    struct task *t = current_task();
    if (t == &tasks[0])
        panic("idle task exited");
    for (int i = 0; i < NFD; i++)
        if (t->fd[i]) {
            file_put(t->fd[i]);
            t->fd[i] = NULL;
        }
    if (t->cwd) {
        iput(t->cwd);
        t->cwd = NULL;
    }
    if (t->pgd) {
        vm_activate(NULL);
        vm_free(t->pgd);
        t->pgd = NULL;
    }
    // Reparent children to init; zombies nobody can reap are released.
    struct task *heir = (init_task && init_task != t) ? init_task : NULL;
    for (int i = 1; i < MAX_TASKS; i++) {
        struct task *c = &tasks[i];
        if (c->state == TASK_UNUSED || c->parent != t)
            continue;
        c->parent = heir;
        if (c->state == TASK_ZOMBIE) {
            if (!heir)
                c->state = TASK_UNUSED;
            else
                wakeup(heir);
        }
    }
    t->exit_status = status;
    t->state = TASK_ZOMBIE;
    if (t->parent) {
        signal_task(t->parent, SIGCHLD);
        wakeup(t->parent);
    }
    if (t == init_task) {
        // All user memory is back by now; only init's own kernel stack is outstanding.
        kprintf("[KERNEL] init exited, status %d, free pages %lu (+1 kstack pending)\n",
                status, page_free_count());
        init_task = NULL;
    }
    schedule();
    panic("zombie rescheduled");
}

// wait4(pid, status, options, rusage): pid > 0 that child, -1 any child, 0 our
// process group, < -1 group -pid. WNOHANG honoured; other options ignored (no
// stopped children exist); rusage is not filled.
long sys_wait4(int pid, uint64_t ustatus, int options)
{
    struct task *self = current_task();
    if (ustatus && !vm_check(self->pgd, ustatus, sizeof(int), 1))
        return -EFAULT;
    for (;;) {
        int have = 0;
        for (int i = 1; i < MAX_TASKS; i++) {
            struct task *c = &tasks[i];
            if (c->state == TASK_UNUSED || c->parent != self)
                continue;
            if (pid > 0 ? c->pid != pid : pid == 0 ? c->pgid != self->pgid :
                pid < -1 ? c->pgid != -pid : 0)
                continue;
            have = 1;
            if (c->state == TASK_ZOMBIE) {
                // Zombie's kstack was freed in schedule_tail before we could run.
                int cpid = c->pid;
                if (ustatus)
                    *(int *)ustatus = c->exit_status;
                c->parent = NULL;
                c->state = TASK_UNUSED;
                return cpid;
            }
        }
        if (!have)
            return -ECHILD;
        if (options & WNOHANG)
            return 0;
        if (sleep_on(self)) // woken by do_exit of a child
            return -EINTR;
    }
}

// Mark a user task for death; it exits with `sig` on its next return to EL0.
static void signal_task(struct task *t, int sig)
{
    if (!t->pgd || t->state == TASK_ZOMBIE || t->state == TASK_UNUSED || t == init_task)
        return;
    uint64_t h = sig == SIGKILL ? SIG_DFL : t->sighand[sig];
    // Default action "ignore" (and the stop signals: no job stopping, so ignored too).
    uint32_t dfl_ign = 1u << SIGCHLD | 1u << SIGCONT | 1u << SIGURG | 1u << SIGWINCH |
                       1u << SIGSTOP | 1u << SIGTSTP | 1u << SIGTTIN | 1u << SIGTTOU;
    if (h == SIG_IGN || (h == SIG_DFL && (dfl_ign >> sig & 1)))
        return;
    if (h != SIG_DFL)
        t->sigpend |= 1u << sig;
    else if (!t->killed)
        t->killed = sig;
    if (t->state == TASK_WAITING && t->wchan)
        t->state = TASK_RUNNABLE;   // sleep_on returns -EINTR
}

void kill_pgrp(int pgid, int sig)
{
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pgid == pgid)
            signal_task(&tasks[i], sig);
}

// kill(pid, sig): pid > 0 one process, 0 our group, -1 everyone but init and
// us, < -1 group -pid. sig 0 only checks existence.
long sys_kill(int pid, int sig)
{
    if (sig < 0 || sig > 31)
        return -EINVAL;
    struct task *self = current_task();
    int found = 0;
    for (int i = 1; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_UNUSED || t->state == TASK_ZOMBIE || !t->pgd)
            continue;
        int match = pid > 0 ? t->pid == pid : pid == 0 ? t->pgid == self->pgid :
                    pid == -1 ? t != self && t != init_task : t->pgid == -pid;
        if (!match)
            continue;
        if (pid > 0 && t == init_task)
            return -EPERM;
        found = 1;
        if (sig)
            signal_task(t, sig);
    }
    return found ? 0 : -ESRCH;
}

long sys_setpgid(int pid, int pgid)
{
    struct task *self = current_task(), *t = NULL;
    if (pid == 0)
        pid = self->pid;
    if (pgid == 0)
        pgid = pid;
    if (pgid < 0)
        return -EINVAL;
    for (int i = 1; i < MAX_TASKS && !t; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid)
            t = &tasks[i];
    if (!t || t->state == TASK_ZOMBIE)
        return -ESRCH;
    if (t != self && t->parent != self)
        return -EPERM;
    t->pgid = pgid;
    return 0;
}

long sys_getprocs(uint64_t ubuf, int max)
{
    if (max < 0 || !vm_check(current_task()->pgd, ubuf, (uint64_t)max * sizeof(struct procinfo), 1))
        return -EFAULT;
    struct procinfo *out = (struct procinfo *)ubuf;
    int n = 0;
    for (int i = 0; i < MAX_TASKS && n < max; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_UNUSED || (i && t->state == TASK_WAITING && !t->wchan))
            continue;   // free, or reserved mid-creation
        struct procinfo *p = &out[n++];
        p->pid = t->pid;
        p->ppid = t->parent ? t->parent->pid : 0;
        p->pgid = t->pgid;
        p->state = t->state == TASK_ZOMBIE ? 'Z' : t->state == TASK_WAITING ? 'S' : 'R';
        memset(p->name, 0, sizeof p->name);
        memcpy(p->name, t->name, sizeof p->name - 1);
    }
    return n;
}

// Heap grows up from the end of the program image toward the stack (one guard
// page between). Returns the new break, or the old one on failure (Linux ABI).
// shortcut: shrinking only moves the break; pages stay mapped until exit.
long sys_brk(uint64_t addr)
{
    struct task *t = current_task();
    if (addr < t->brk_base || addr > t->mmap_low - PAGE_SIZE)    // guard page below mmaps
        return (long)t->brk;
    for (; t->brk_mapped < addr; t->brk_mapped += PAGE_SIZE)
        if (!vm_alloc_page(t->pgd, t->brk_mapped, VM_NX))
            return (long)t->brk;
    t->brk = addr;
    return (long)addr;
}

// ---- signal handlers ----
// Delivery pushes a sigframe on the user stack and enters the handler with
// x0 = sig (x1 = siginfo, x2 = 0 for SA_SIGINFO), lr = the sa_restorer, which
// must call rt_sigreturn to pop the frame. The signal and sa_mask stay blocked
// while the handler runs.
// shortcut: no SA_RESTART, siginfo carries only si_signo, no ucontext or
// alternate stack; syscalls interrupted by a caught signal return -EINTR.

struct sigframe {
    struct fpstate fp;
    struct trap_frame tf;
    uint32_t mask;              // sigmask to restore
    uint32_t pad[3];
    int32_t siginfo[32];        // si_signo, si_errno, si_code, ... (128 bytes)
};

// Linux sigset bit n-1 = signal n; ours is bit n.
static uint32_t from_sigset(uint64_t set) { return (uint32_t)(set << 1) & ~(1u << SIGKILL | 1u << SIGSTOP | 1u); }
static uint64_t to_sigset(uint32_t mask) { return mask >> 1; }

long sys_rt_sigaction(int sig, uint64_t uact, uint64_t uold, uint64_t size)
{
    struct task *t = current_task();
    if (sig < 1 || sig > 31 || size != 8)
        return -EINVAL;
    if ((uact && !vm_check(t->pgd, uact, sizeof(struct k_sigaction), 0)) ||
        (uold && !vm_check(t->pgd, uold, sizeof(struct k_sigaction), 1)))
        return -EFAULT;
    struct k_sigaction old = { t->sighand[sig], t->sigflags[sig], t->sigrestorer,
                               to_sigset(t->sigactmask[sig]) };
    if (uact) {
        struct k_sigaction act = *(struct k_sigaction *)uact;
        if (sig == SIGKILL || sig == SIGSTOP)
            return -EINVAL;
        if (act.handler > SIG_IGN && !(act.flags & SA_RESTORER))
            return -EINVAL;     // no kernel-provided trampoline
        t->sighand[sig] = act.handler;
        t->sigflags[sig] = (uint32_t)act.flags;
        t->sigactmask[sig] = from_sigset(act.mask);
        if (act.handler > SIG_IGN)
            t->sigrestorer = act.restorer;
        if (act.handler == SIG_IGN)
            t->sigpend &= ~(1u << sig);
    }
    if (uold)
        *(struct k_sigaction *)uold = old;
    return 0;
}

long sys_rt_sigprocmask(int how, uint64_t uset, uint64_t uold, uint64_t size)
{
    struct task *t = current_task();
    if (size != 8)
        return -EINVAL;
    if ((uset && !vm_check(t->pgd, uset, 8, 0)) || (uold && !vm_check(t->pgd, uold, 8, 1)))
        return -EFAULT;
    uint64_t old = to_sigset(t->sigmask);
    if (uset) {
        uint32_t m = from_sigset(*(uint64_t *)uset);
        if (how == SIG_BLOCK)
            t->sigmask |= m;
        else if (how == SIG_UNBLOCK)
            t->sigmask &= ~m;
        else if (how == SIG_SETMASK)
            t->sigmask = m;
        else
            return -EINVAL;
    }
    if (uold)
        *(uint64_t *)uold = old;
    return 0;
}

// Called on every return to EL0 with tf = the frame about to be restored.
void signal_deliver(struct trap_frame *tf)
{
    struct task *t = current_task();
    if (t->killed)
        do_exit(t->killed);
    uint32_t ready = t->sigpend & ~t->sigmask;
    if (!ready)
        return;
    int sig = __builtin_ctz(ready);
    t->sigpend &= ~(1u << sig);
    uint64_t sp = (tf->sp_el0 - sizeof(struct sigframe)) & ~15ul;
    if (t->sighand[sig] <= SIG_IGN || !vm_check(t->pgd, sp, sizeof(struct sigframe), 1))
        do_exit(SIGSEGV);       // no handler any more, or no room for the frame
    struct sigframe *f = (struct sigframe *)sp;
    fpsimd_save(&f->fp);        // user FP state is still live in the registers
    f->tf = *tf;
    f->mask = t->sigmask;
    memset(f->siginfo, 0, sizeof f->siginfo);
    f->siginfo[0] = sig;
    t->sigmask |= 1u << sig | t->sigactmask[sig];
    tf->x[0] = (uint64_t)sig;
    tf->x[1] = t->sigflags[sig] & SA_SIGINFO ? sp + __builtin_offsetof(struct sigframe, siginfo) : 0;
    tf->x[2] = 0;
    tf->x[30] = t->sigrestorer;
    tf->elr = t->sighand[sig];
    tf->sp_el0 = sp;
}

long sys_rt_sigreturn(struct trap_frame *tf)
{
    struct task *t = current_task();
    uint64_t sp = tf->sp_el0;
    if ((sp & 15) || !vm_check(t->pgd, sp, sizeof(struct sigframe), 0))
        do_exit(SIGSEGV);
    struct sigframe *f = (struct sigframe *)sp;
    *tf = f->tf;
    tf->spsr &= 0xf0000000;     // user may only restore NZCV: stay in EL0t, IRQs on
    t->sigmask = f->mask & ~(1u << SIGKILL | 1u << SIGSTOP);
    fpsimd_load(&f->fp);
    return (long)tf->x[0];      // dispatcher writes this back into x0
}

// ---- memory mappings: anonymous only, carved downward from below the stack ----
// shortcut: no file mappings (-ENODEV); munmap'd ranges are not reused unless
// mapped MAP_FIXED; MAP_SHARED is private (fork copies it); mprotect is a no-op;
// PROT_NONE (musl's guard pages) is simply unmapped.

long sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, int64_t off)
{
    (void)fd, (void)off;
    struct task *t = current_task();
    if (!(flags & MAP_ANONYMOUS))
        return -ENODEV;
    if (!len || len > USER_STACK_BASE)
        return -EINVAL;
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (flags & MAP_FIXED) {
        if ((addr & (PAGE_SIZE - 1)) || addr < USER_BASE || addr + len > USER_STACK_BASE - PAGE_SIZE)
            return -EINVAL;
        if (!(prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) {     // PROT_NONE guard: leave unmapped
            for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE)
                vm_unmap(t->pgd, va);
            return (long)addr;
        }
    } else {
        addr = t->mmap_low - len;
        if (t->mmap_low < len || addr < t->brk_mapped + PAGE_SIZE)
            return -ENOMEM;
    }
    uint64_t vmf = (prot & PROT_WRITE ? 0 : VM_RO) | (prot & PROT_EXEC ? 0 : VM_NX);
    for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
        vm_unmap(t->pgd, va);           // MAP_FIXED replaces; fresh pages are zeroed
        if (!vm_alloc_page(t->pgd, va, vmf)) {
            for (uint64_t u = addr; u < va; u += PAGE_SIZE)
                vm_unmap(t->pgd, u);
            return -ENOMEM;
        }
    }
    if (addr < t->mmap_low)
        t->mmap_low = addr;
    if (prot & PROT_EXEC)
        icache_inval_all();
    return (long)addr;
}

long sys_munmap(uint64_t addr, uint64_t len)
{
    struct task *t = current_task();
    if ((addr & (PAGE_SIZE - 1)) || addr < USER_BASE || addr + len > USER_STACK_BASE - PAGE_SIZE)
        return -EINVAL;
    for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE)
        vm_unmap(t->pgd, va);
    return 0;
}

// clone() for fork and vfork only (no threads). vfork's CLONE_VM is emulated
// by copying, which is fine for the vfork-then-exec pattern.
long sys_clone(struct trap_frame *tf, uint64_t flags, uint64_t stack)
{
    if ((flags & 0xff) != SIGCHLD || (flags & ~(0xfful | CLONE_VM | CLONE_VFORK)))
        return -EINVAL;
    long pid = sys_fork(tf);
    if (pid > 0 && stack) {
        for (int i = 1; i < MAX_TASKS; i++)
            if (tasks[i].pid == pid)
                task_frame(&tasks[i])->sp_el0 = stack;
    }
    return pid;
}

long sys_uname(uint64_t ubuf)
{
    if (!vm_check(current_task()->pgd, ubuf, sizeof(struct utsname), 1))
        return -EFAULT;
    struct utsname *u = (struct utsname *)ubuf;
    memset(u, 0, sizeof *u);
    memcpy(u->sysname, "PiOS", 5);
    memcpy(u->nodename, "pios", 5);
    memcpy(u->release, PIOS_VERSION, sizeof PIOS_VERSION);
    memcpy(u->version, "#1", 3);
    memcpy(u->machine, "aarch64", 8);
    return 0;
}

long sys_getpgid(int pid)
{
    if (pid == 0)
        return current_task()->pgid;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid)
            return tasks[i].pgid;
    return -ESRCH;
}

long sys_setsid(void)       // no sessions: just a new process group
{
    struct task *t = current_task();
    t->pgid = t->pid;
    return t->pid;
}

long sys_getppid(void)
{
    struct task *p = current_task()->parent;
    return p ? p->pid : (init_task ? init_task->pid : 0);
}

long sys_umask(int mask)
{
    struct task *t = current_task();
    long old = t->umask;
    t->umask = (uint32_t)mask & 0777;
    return old;
}

// ---- ITIMER_REAL: SIGALRM from the timer tick ----

static uint64_t to_ticks(int64_t sec, int64_t usec)
{
    uint64_t us = (uint64_t)sec * 1000000 + (uint64_t)usec, t = 1000000 / HZ;
    return (us + t - 1) / t;
}

static void from_ticks(uint64_t ticks, int64_t *sec, int64_t *usec)
{
    *sec = (int64_t)(ticks / HZ);
    *usec = (int64_t)(ticks % HZ) * (1000000 / HZ);
}

static void get_itimer(struct task *t, struct itimerval *v)
{
    uint64_t now = timer_ticks(), left = t->alarm_at > now ? t->alarm_at - now : 0;
    from_ticks(t->alarm_every, &v->interval_sec, &v->interval_usec);
    from_ticks(t->alarm_at ? left : 0, &v->value_sec, &v->value_usec);
}

long sys_getitimer(int which, uint64_t uval)
{
    struct task *t = current_task();
    if (which != 0)
        return -EINVAL;
    if (!vm_check(t->pgd, uval, sizeof(struct itimerval), 1))
        return -EFAULT;
    get_itimer(t, (struct itimerval *)uval);
    return 0;
}

long sys_setitimer(int which, uint64_t unew, uint64_t uold)
{
    struct task *t = current_task();
    if (which != 0)
        return -EINVAL;     // shortcut: no ITIMER_VIRTUAL / ITIMER_PROF
    if (!vm_check(t->pgd, unew, sizeof(struct itimerval), 0) ||
        (uold && !vm_check(t->pgd, uold, sizeof(struct itimerval), 1)))
        return -EFAULT;
    struct itimerval nv = *(struct itimerval *)unew;
    if (uold)
        get_itimer(t, (struct itimerval *)uold);
    uint64_t v = to_ticks(nv.value_sec, nv.value_usec);
    t->alarm_at = v ? timer_ticks() + v : 0;
    t->alarm_every = to_ticks(nv.interval_sec, nv.interval_usec);
    return 0;
}

// From the timer IRQ.
void itimer_tick(uint64_t now)
{
    for (int i = 1; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (!t->alarm_at || now < t->alarm_at || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE)
            continue;
        t->alarm_at = t->alarm_every ? now + t->alarm_every : 0;
        signal_task(t, SIGALRM);
    }
}
