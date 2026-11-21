// Process lifecycle: init creation, fork, execve, exit, waitpid.
#include <stddef.h>
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "sched.h"
#include "uapi.h"
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

    // Stack top page holds strings then the argv/envp pointer arrays below them.
    uint64_t str_va = USER_STACK_TOP - used;
    uint64_t ptr_va = (str_va - (uint64_t)(argc + envc + 2) * 8) & ~15UL;
    uint64_t *pgd = ptr_va < USER_STACK_TOP - PAGE_SIZE ? NULL : vm_new();
    if (!pgd) {
        iput(ino);
        return ptr_va < USER_STACK_TOP - PAGE_SIZE ? -E2BIG : -ENOMEM;
    }
    uint64_t entry, img_top;
    err = elf_load(pgd, ino, &entry, &img_top);
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
    uint64_t *ptrs = (uint64_t *)STACK_K(ptr_va);
#undef STACK_K
    uint64_t s = str_va;
    for (long i = 0; i < argc + envc; i++) {
        if (i == argc)
            *ptrs++ = 0;    // argv terminator
        *ptrs++ = s;
        s += strlen(kbuf + (s - str_va)) + 1;
    }
    if (envc == 0)
        *ptrs++ = 0;        // argv terminator (loop never reached i == argc)
    *ptrs = 0;              // envp terminator

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

    t->brk_base = t->brk = t->brk_mapped = img_top;
    for (int i = 0; i < 32; i++)        // handlers point into the old image; ignores survive (POSIX)
        if (t->sighand[i] != SIG_IGN)
            t->sighand[i] = SIG_DFL;
    t->sigpend = 0;
    memset(&t->fp, 0, sizeof t->fp);
    if (t == current_task())
        fpsimd_load(&t->fp);
    memset(tf, 0, sizeof *tf);
    tf->elr = entry;
    tf->spsr = 0;           // EL0t, interrupts unmasked
    tf->sp_el0 = ptr_va;
    tf->x[0] = (uint64_t)argc;
    tf->x[1] = ptr_va;
    tf->x[2] = ptr_va + (uint64_t)(argc + 1) * 8;
    return argc;            // dispatcher stores this in x0, which must be argc
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
    c->brk_base = p->brk_base, c->brk = p->brk, c->brk_mapped = p->brk_mapped;
    for (int i = 0; i < NFD; i++)
        if ((c->fd[i] = p->fd[i]))
            c->fd[i]->refs++;
    if ((c->cwd = p->cwd))
        iget(c->cwd);
    c->state = TASK_RUNNABLE;
    return c->pid;
}

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
    if (t->parent)
        wakeup(t->parent);
    if (t == init_task) {
        // All user memory is back by now; only init's own kernel stack is outstanding.
        kprintf("[KERNEL] init exited, status %d, free pages %lu (+1 kstack pending)\n",
                status, page_free_count());
        init_task = NULL;
    }
    schedule();
    panic("zombie rescheduled");
}

long sys_waitpid(int pid, uint64_t ustatus, int options)
{
    struct task *self = current_task();
    if (options != 0)
        return -EINVAL;
    if (ustatus && !vm_check(self->pgd, ustatus, sizeof(int), 1))
        return -EFAULT;
    for (;;) {
        int have = 0;
        for (int i = 1; i < MAX_TASKS; i++) {
            struct task *c = &tasks[i];
            if (c->state == TASK_UNUSED || c->parent != self || (pid != -1 && c->pid != pid))
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
    if (h == SIG_IGN)
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

long sys_kill(int pid, int sig)
{
    if (sig < 1 || sig > 31)
        return -EINVAL;
    for (int i = 1; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_UNUSED || t->pid != pid)
            continue;
        if (t == init_task || !t->pgd)
            return -EPERM;
        signal_task(t, sig);
        return 0;
    }
    return -ESRCH;
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
    if (addr < t->brk_base || addr > USER_STACK_BASE - PAGE_SIZE)
        return (long)t->brk;
    for (; t->brk_mapped < addr; t->brk_mapped += PAGE_SIZE)
        if (!vm_alloc_page(t->pgd, t->brk_mapped, VM_NX))
            return (long)t->brk;
    t->brk = addr;
    return (long)addr;
}

// ---- signal handlers ----
// Delivery pushes a sigframe on the user stack and enters the handler with
// x0 = sig, lr = restorer; the restorer's sigreturn pops the frame. The
// signal stays blocked while its handler runs.
// shortcut: no SA_RESTART, siginfo or alternate stack; syscalls interrupted by a caught signal return -EINTR.

struct sigframe {
    struct fpstate fp;
    struct trap_frame tf;
    uint32_t mask;              // sigmask to restore
};

long sys_sigaction(int sig, uint64_t handler, uint64_t restorer)
{
    struct task *t = current_task();
    if (sig < 1 || sig > 31 || sig == SIGKILL)
        return -EINVAL;
    uint64_t old = t->sighand[sig];
    t->sighand[sig] = handler;
    if (handler > SIG_IGN)
        t->sigrestorer = restorer;
    t->sigpend &= ~(1u << sig);
    return (long)old;
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
    t->sigmask |= 1u << sig;
    tf->x[0] = (uint64_t)sig;
    tf->x[30] = t->sigrestorer;
    tf->elr = t->sighand[sig];
    tf->sp_el0 = sp;
}

long sys_sigreturn(struct trap_frame *tf)
{
    struct task *t = current_task();
    uint64_t sp = tf->sp_el0;
    if ((sp & 15) || !vm_check(t->pgd, sp, sizeof(struct sigframe), 0))
        do_exit(SIGSEGV);
    struct sigframe *f = (struct sigframe *)sp;
    *tf = f->tf;
    tf->spsr &= 0xf0000000;     // user may only restore NZCV: stay in EL0t, IRQs on
    t->sigmask = f->mask & ~(1u << SIGKILL);
    fpsimd_load(&f->fp);
    return (long)tf->x[0];      // dispatcher writes this back into x0
}
