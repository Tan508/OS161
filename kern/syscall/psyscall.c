#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <current.h>
#include <proc.h>
#include <synch.h>
#include <addrspace.h>
#include <vfs.h>
#include <vnode.h>
#include <pid.h>
#include <filetable.h>
#include <mips/trapframe.h>
#include <syscall.h>
#include <copyinout.h>
#include <limits.h>
#include <kern/wait.h>
#include <proc.h>

void enter_forked_process(void *child_trapframe, unsigned long unused);

int
sys_fork(struct trapframe *parent_tf, pid_t *retval)
{
    struct proc *child;
    struct addrspace *child_as;
    struct trapframe *child_tf;
    pid_t child_pid;
    int err;

    KASSERT(curproc != NULL);
    KASSERT(parent_tf != NULL);
    KASSERT(retval != NULL);

    /* Create child process struct */
    child = proc_create(curproc->p_name);
    if (child == NULL) {
        return ENOMEM;
    }

    /* Allocate PID for child */
    err = pid_alloc(child, &child_pid);
    if (err) {
        proc_destroy(child);
        return err;
    }
    child->pid  = child_pid;
    child->ppid = curproc->pid;

    /* Copy address space */
    KASSERT(curproc->p_addrspace != NULL);
    err = as_copy(curproc->p_addrspace, &child_as);
    if (err) {
        pid_forget(child_pid);
        proc_destroy(child);
        return err;
    }
    child->p_addrspace = child_as;
    
    /* Copy CWD */
    if (curproc->p_cwd != NULL) {
        VOP_INCREF(curproc->p_cwd);
        child->p_cwd = curproc->p_cwd;
    }

    /* Shallow-copy file table (shared fds) */
    err = ft_shallow_copy(curproc, child);
    if (err) {
        if (child->p_cwd) {
            VOP_DECREF(child->p_cwd);
            child->p_cwd = NULL;
        }

        if (child->p_addrspace) {
            as_destroy(child->p_addrspace);
            child->p_addrspace = NULL;
        }

        pid_forget(child_pid);
        proc_destroy(child);
        return err;
    }

    /* Copy trapframe for child */
    child_tf = kmalloc(sizeof(struct trapframe));
    if (child_tf == NULL) {
        ft_destroy(child->proc_ft);
        child->proc_ft = NULL;

        if (child->p_cwd) {
            VOP_DECREF(child->p_cwd);
            child->p_cwd = NULL;
        }
        if (child->p_addrspace) {
            as_destroy(child->p_addrspace);
            child->p_addrspace = NULL;
        }
        pid_forget(child_pid);
        proc_destroy(child);
        return ENOMEM;
    }
    *child_tf = *parent_tf;

    /* Spawn child thread */
    err = thread_fork(curproc->p_name, child, enter_forked_process, child_tf, 0);
    if (err) {
        kfree(child_tf);
        ft_destroy(child->proc_ft);
        child->proc_ft = NULL;

        if (child->p_cwd != NULL) {
            VOP_DECREF(child->p_cwd);
            child->p_cwd = NULL;
        }
        if (child->p_addrspace != NULL) {
            as_destroy(child->p_addrspace);
            child->p_addrspace = NULL;
        }
        pid_forget(child_pid);
        proc_destroy(child);
        return err;
    }

    /* Parent returns child's PID */
    *retval = child_pid;
    return 0;
}

int sys_getpid(pid_t *retval)
{
 	KASSERT(curproc != NULL);
    KASSERT(curproc->pid != 0);

    *retval = curproc->pid;
    return 0;
}

int sys__exit(int exitcode)
{
    struct proc *proc = curproc;
    KASSERT(proc != NULL);

    int status = _MKWAIT_EXIT(exitcode);

    /* Update PID table entry and wake up waitpid */
    struct pid_entry *pid_entry = pid_lookup(proc->pid);
    if (pid_entry != NULL) {
        lock_acquire(pid_entry->lock);
        pid_entry->exitcode = status;
        pid_entry->state = PID_ZOMBIE;  /* no longer ALIVE */
        cv_broadcast(pid_entry->cv, pid_entry->lock);  /* wake any waitpid callers */
        lock_release(pid_entry->lock);
    }

    /* Update proc structure for any waitpid in this process */
    lock_acquire(proc->p_waitlock);
    proc->p_exitcode = status;
    proc->p_exited = true;
    cv_broadcast(proc->p_waitcv, proc->p_waitlock);
    lock_release(proc->p_waitlock);

    /* Destroy user resources */
    if (proc->p_addrspace) {
        as_deactivate();
        struct addrspace *old = proc->p_addrspace;
        proc->p_addrspace = NULL;
        as_destroy(old);
    }

    if (proc->p_cwd) {
        VOP_DECREF(proc->p_cwd);
        proc->p_cwd = NULL;
    }

    if (proc->proc_ft) {
        ft_destroy(proc->proc_ft);
        proc->proc_ft = NULL;
    }

    /* Finish the thread; proc itself will be destroyed in waitpid via proc_destroy */
    thread_exit();
    panic("sys__exit: thread_exit returned\n");
}

int sys_waitpid(pid_t pid, userptr_t statusptr, int options, pid_t *retval)
{
    if (options != 0) {
        return EINVAL;
    }

    struct pid_entry *pid_entry = pid_lookup(pid);
    if (!pid_entry) {
        return ESRCH;
    }

    /* Only parent may wait */
    lock_acquire(pid_entry->lock);
    if (pid_entry->ppid != curproc->pid) {
        lock_release(pid_entry->lock);
        return ECHILD;
    }

    while (pid_entry->state == PID_ALIVE) {
        cv_wait(pid_entry->cv, pid_entry->lock); /* wakeup happens in sys__exit under same lock */
    }

    int status = pid_entry->exitcode;
    lock_release(pid_entry->lock);

    if (statusptr != (userptr_t)NULL) {
        int err = copyout(&status, statusptr, sizeof(status));
        if (err) {
            return err;
        }
    }

    /* Reap: free PID entry and the proc */
    struct proc *child = pid_get_proc(pid); /* might be NULL since we set it NULL in exit */
    pid_forget(pid);
    if (child) proc_destroy(child);

    *retval = pid;
    return 0;
}