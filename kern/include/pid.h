#ifndef _PID_H_
#define _PID_H_

#include <types.h>
#include <limits.h>

struct proc;
struct lock;
struct cv;

enum pid_state { PID_ALIVE, PID_ZOMBIE, PID_REAPED };

struct pid_entry {
    pid_t pid;
    pid_t ppid;
    int exitcode;
    enum pid_state state;
    struct lock *lock;
    struct cv *cv;
    struct proc *proc; /* valid while ALIVE */
};

/*
 * PID allocation/management interface
 */
void    pid_bootstrap(void);

/* Allocate a new PID for process p; store it in *ret */
int     pid_alloc(struct proc *p, pid_t *ret);

/* Remove the mapping for pid so it can be reused */
void    pid_forget(pid_t pid);

/* Look up the pid_entry for a pid */
struct pid_entry *pid_lookup(pid_t pid);

/* Look up the struct proc* for a pid */
struct proc *pid_get_proc(pid_t pid);

static inline struct lock *pid_lock(struct pid_entry *e) { return e->lock; }
static inline struct cv   *pid_cv(struct pid_entry *e)   { return e->cv;  }

#endif /* _PID_H_ */
