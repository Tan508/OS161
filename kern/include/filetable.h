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

#endif  /*FILETABLE_H*/
