#include <types.h>
#include <lib.h>
#include <synch.h>
#include <kern/errno.h>
#include <proc.h>
#include <current.h>
#include <copyinout.h>
#include <thread.h>
#include <addrspace.h>
#include <pid.h>
#include <filetable.h>
#include <limits.h>

static struct pid_entry *pidtab[PID_MAX];
static struct lock *pidtab_lock;

static int
pid_alloc_slot(void)
{
    for (int i = 2; i < PID_MAX; i++) { 
        if (pidtab[i] == NULL) return i;
    }
    return -1;
}

void pid_bootstrap(void) {
    pidtab_lock = lock_create("pidtab_lock");
    KASSERT(pidtab_lock);
    memset(pidtab, 0, sizeof(pidtab));
}

int
pid_alloc(struct proc *p, pid_t *out_pid)
{
    KASSERT(out_pid != NULL);
    lock_acquire(pidtab_lock);

    int idx = pid_alloc_slot();
    if (idx < 0) {
        lock_release(pidtab_lock);
        return EAGAIN;
    }

    struct pid_entry *e = kmalloc(sizeof(*e));
    if (e == NULL) {
        lock_release(pidtab_lock);
        return ENOMEM;
    }

    e->lock = lock_create("pid_lock");
    e->cv   = cv_create("pid_cv");
    if (e->lock == NULL || e->cv == NULL) {
        if (e->lock) lock_destroy(e->lock);
        if (e->cv)   cv_destroy(e->cv);
        kfree(e);
        lock_release(pidtab_lock);
        return ENOMEM;
    }

    e->pid = idx;
    e->ppid = (curproc ? curproc->pid : 0);
    e->exitcode = 0;
    e->state = PID_ALIVE;
    e->proc = p;

    pidtab[idx] = e;
    *out_pid = idx;

    lock_release(pidtab_lock);
    return 0;
}

void pid_forget(pid_t pid) 
{
    if (pid <= 0 || pid >= PID_MAX) return;

    lock_acquire(pidtab_lock);
    struct pid_entry *e = pidtab[pid];
    if (e != NULL) {
        /* Caller must ensure no one holds e->lock here */
        pidtab[pid] = NULL;
        if (e->lock) lock_destroy(e->lock);
        if (e->cv)   cv_destroy(e->cv);
        kfree(e);
    }
    lock_release(pidtab_lock);
}

struct pid_entry *pid_lookup(pid_t pid) 
{
    if (pid <= 0 || pid >= PID_MAX) {
        return NULL;
    }

    return pidtab[pid];
}

struct proc *pid_get_proc(pid_t pid) 
{
    if (pid <= 0 || pid >= PID_MAX) {
        return NULL;
    }

    struct pid_entry *e = pid_lookup(pid);
    if (e == NULL) {
        return NULL;
    }

    struct proc *p = NULL;
    lock_acquire(e->lock);
    p = e->proc;        /* NULL if ZOMBIE/REAPED; valid only while ALIVE */
    lock_release(e->lock);
    return p;
}

