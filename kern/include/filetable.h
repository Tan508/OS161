#ifndef _FILETABLE_H_
#define _FILETABLE_H_

#include <synch.h>
#include <limits.h>
#include <vnode.h>
#include <lib.h>

struct filetable {
    int refcnt;
    struct vnode *vn;
    off_t offset;
    int flags;
    struct lock *lock;
};

struct ft {
    struct filetable *entries[OPEN_MAX];
    struct lock *ft_lock;
};

/* Helpers */
int  ft_shallow_copy(struct proc *parent, struct proc *child);
void ft_destroy(struct ft *table);

#endif  /*FILETABLE_H*/
