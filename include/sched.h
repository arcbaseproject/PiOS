#pragma once
#include <stdint.h>

struct file;
struct inode;

// Callee-saved state per AAPCS64; layout must match switch.S.
struct cpu_context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t fp, lr, sp;
};

enum task_state { TASK_UNUSED, TASK_RUNNABLE, TASK_RUNNING, TASK_WAITING, TASK_ZOMBIE };

struct task {
    struct cpu_context ctx;     // must stay first (switch.S takes &task->ctx == task)
    enum task_state state;
    int pid;
    char name[16];
    void *kstack;               // one page; NULL for idle, and for zombies once switched out
    uint64_t *pgd;              // user page table; NULL = kernel thread
    struct task *parent;        // NULL = nobody waits (kernel threads, orphans)
    int exit_status;            // waitpid status encoding
    void *wchan;                // what a WAITING task sleeps on
    struct file *fd[16];        // NFD in vfs.h
    struct inode *cwd;          // NULL = root (kernel threads)
};

#define MAX_TASKS 64
extern struct task tasks[MAX_TASKS];

void sched_init(void);
struct task *task_alloc(const char *name);
void task_release(struct task *t);
int kthread_create(void (*fn)(void *), void *arg, const char *name);
void schedule(void);
void sleep_on(void *chan);
void wakeup(void *chan);
_Noreturn void task_exit(void);
struct task *current_task(void);
void set_task_name(struct task *t, const char *name);
