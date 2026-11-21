// Round-robin preemptive scheduler, single core. schedule() is called from the
// timer IRQ (every tick = one timeslice) and from task_exit.
#include <stddef.h>
#include "kernel.h"
#include "mm.h"
#include "proc.h"
#include "sched.h"
#include "uapi.h"

extern void cpu_switch(struct cpu_context *prev, struct cpu_context *next);
extern void task_entry(void);

struct task tasks[MAX_TASKS];
static struct task *current;
static struct task *reap_pending;   // zombie whose stack is freed once we are off it
static int next_pid = 1;

void sched_init(void)
{
    // The boot thread becomes the idle task (pid 0) and keeps the boot stack.
    tasks[0].state = TASK_RUNNING;
    tasks[0].pid = 0;
    memcpy(tasks[0].name, "idle", 5);
    current = &tasks[0];
}

struct task *current_task(void) { return current; }

static void set_name(struct task *t, const char *name)
{
    size_t i = 0;
    for (; name[i] && i < sizeof t->name - 1; i++)
        t->name[i] = name[i];
    t->name[i] = 0;
}

// Reserve a slot with a fresh kernel stack. Call with IRQs masked.
struct task *task_alloc(const char *name)
{
    for (int i = 1; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state != TASK_UNUSED)
            continue;
        void *stack = page_alloc();
        if (!stack)
            return NULL;
        memset(t, 0, sizeof *t);
        t->kstack = stack;
        t->pid = next_pid++;
        t->state = TASK_WAITING;    // reserved, not runnable until caller sets it
        set_name(t, name);
        return t;
    }
    return NULL;
}

void task_release(struct task *t)
{
    page_free(t->kstack);
    t->kstack = NULL;
    t->state = TASK_UNUSED;
}

void set_task_name(struct task *t, const char *name) { set_name(t, name); }

int kthread_create(void (*fn)(void *), void *arg, const char *name)
{
    uint64_t daif = read_sysreg(daif);
    irq_disable();
    int pid = -1;
    struct task *t = task_alloc(name);
    if (t) {
        t->ctx.x19 = (uintptr_t)fn;
        t->ctx.x20 = (uintptr_t)arg;
        t->ctx.lr = (uintptr_t)task_entry;
        t->ctx.sp = (uintptr_t)t->kstack + PAGE_SIZE;   // page aligned, so 16-byte aligned
        t->state = TASK_RUNNABLE;
        pid = t->pid;
    }
    write_sysreg(daif, daif);
    return pid;
}

// Runs on the new task's stack right after a switch: safe to free the old zombie's.
void schedule_tail(void)
{
    if (reap_pending) {
        page_free(reap_pending->kstack);
        reap_pending->kstack = NULL;
        if (!reap_pending->parent)      // nobody will waitpid() for it
            reap_pending->state = TASK_UNUSED;
        reap_pending = NULL;
    }
}

void schedule(void)
{
    uint64_t daif = read_sysreg(daif);
    irq_disable();

    int cur = (int)(current - tasks);
    struct task *next = &tasks[0];
    for (int n = 1; n < MAX_TASKS; n++) {
        int i = (cur + n) % MAX_TASKS;
        if (i != 0 && tasks[i].state == TASK_RUNNABLE) {
            next = &tasks[i];
            break;
        }
    }
    if (next == &tasks[0] && current != &tasks[0] && current->state == TASK_RUNNING)
        next = current;     // only runnable thread: keep running it, not idle

    if (next != current) {
        struct task *prev = current;
        if (prev->state == TASK_RUNNING)
            prev->state = TASK_RUNNABLE;
        else if (prev->state == TASK_ZOMBIE)
            reap_pending = prev;
        next->state = TASK_RUNNING;
        current = next;
        if (next->pgd != prev->pgd)
            vm_activate(next->pgd);
        fpsimd_save(&prev->fp);
        fpsimd_load(&next->fp);
        cpu_switch(&prev->ctx, &next->ctx);
        schedule_tail();
    }
    write_sysreg(daif, daif);
}

// Block until wakeup(chan). Call with IRQs masked; recheck the condition after.
// Returns -EINTR without sleeping, or on wake, if a signal is pending.
int sleep_on(void *chan)
{
    if (current->killed || (current->sigpend & ~current->sigmask))
        return -EINTR;
    current->wchan = chan;
    current->state = TASK_WAITING;
    schedule();
    current->wchan = NULL;
    return current->killed || (current->sigpend & ~current->sigmask) ? -EINTR : 0;
}

void wakeup(void *chan)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_WAITING && tasks[i].wchan == chan)
            tasks[i].state = TASK_RUNNABLE;
}

// Kernel threads fall off their function into here (see task_entry).
_Noreturn void task_exit(void)
{
    do_exit(0);
}
