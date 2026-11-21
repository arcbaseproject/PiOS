#pragma once
#include <stdint.h>

struct file;
struct inode;

// Callee-saved state per AAPCS64; layout must match switch.S.
struct cpu_context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t fp, lr, sp;
};

// User FP/SIMD registers. Kernel code never touches them (-mgeneral-regs-only),
// so they only need swapping on context switch. Layout matches switch.S.
struct fpstate {
    __uint128_t q[32];
    uint64_t fpsr, fpcr;
} __attribute__((aligned(16)));

void fpsimd_save(struct fpstate *s);
void fpsimd_load(const struct fpstate *s);

enum task_state { TASK_UNUSED, TASK_RUNNABLE, TASK_RUNNING, TASK_WAITING, TASK_ZOMBIE };

struct task {
    struct cpu_context ctx;     // must stay first (switch.S takes &task->ctx == task)
    enum task_state state;
    int pid;
    int pgid;                   // process group (console ^C target)
    int killed;                 // pending fatal signal, acted on at return to EL0
    uint32_t sigpend, sigmask;  // caught signals pending / blocked (bit = 1 << sig)
    uint64_t sighand[32];       // SIG_DFL, SIG_IGN or user handler address
    uint64_t sigrestorer;       // user code that calls rt_sigreturn
    uint32_t sigflags[32];      // sa_flags (SA_SIGINFO honoured)
    uint32_t sigactmask[32];    // sa_mask, blocked while that handler runs
    uint64_t tpidr;             // TPIDR_EL0 (user thread pointer), saved while switched out
    uint64_t mmap_low;          // lowest mmap'd address; mmap grows down from the stack
    uint32_t umask;
    uint64_t alarm_at, alarm_every;  // ITIMER_REAL in ticks (0 = off)
    char name[16];
    void *kstack;               // one page; NULL for idle, and for zombies once switched out
    uint64_t *pgd;              // user page table; NULL = kernel thread
    struct task *parent;        // NULL = nobody waits (kernel threads, orphans)
    int exit_status;            // waitpid status encoding
    void *wchan;                // what a WAITING task sleeps on
    struct file *fd[32];        // NFD in vfs.h
    uint8_t cloexec[32];        // FD_CLOEXEC per slot
    struct inode *cwd;          // NULL = root (kernel threads)
    uint64_t brk_base, brk, brk_mapped;    // heap: start, current break, mapped top
    struct fpstate fp;          // saved while switched out
};

#define MAX_TASKS 64
extern struct task tasks[MAX_TASKS];

void sched_init(void);
struct task *task_alloc(const char *name);
void task_release(struct task *t);
int kthread_create(void (*fn)(void *), void *arg, const char *name);
void schedule(void);
int sleep_on(void *chan);    // -EINTR if killed or a caught signal is pending (caller must bail out)
void wakeup(void *chan);
_Noreturn void task_exit(void);
struct task *current_task(void);
void set_task_name(struct task *t, const char *name);
