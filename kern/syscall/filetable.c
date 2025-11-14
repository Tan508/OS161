#include <types.h>
#include <lib.h>
#include <kern/limits.h>
#include <vnode.h>
#include <vfs.h>
#include <synch.h>
#include <proc.h>
#include <filetable.h>
#include <kern/errno.h>
#include <vnode.h>

void ft_destroy(struct ft *table)
{
    if (table == NULL) {
        return;
    }

    lock_acquire(table->ft_lock);

    for (int i = 0; i < OPEN_MAX; i++) {
        struct filetable *e = table->entries[i];
        if (e == NULL) {
            continue;
        }

        /* Remove the pointer from this ft so we don't double-touch it. */
        table->entries[i] = NULL;

        /* Drop our reference. */
        bool last = false;

        lock_acquire(e->lock);
        KASSERT(e->refcnt > 0);
        e->refcnt--;
        if (e->refcnt == 0) {
            last = true;
        }
        lock_release(e->lock);

        if (last) {
            /* Nobody else is using this file object: close and free it. */
            vfs_close(e->vn);
            lock_destroy(e->lock);
            kfree(e);
        }
    }

    lock_release(table->ft_lock);
    lock_destroy(table->ft_lock);
    kfree(table);
}

/*
 * Shallow-copy parent's file descriptor table into child.
 * Both parent and child have their own 'struct ft', but each
 * entry points to the same underlying 'struct filetable' object.
 * We bump refcnt on each shared filetable.
 */
int ft_shallow_copy(struct proc *parent, struct proc *child)
{
    struct ft *pft, *cft;

    KASSERT(parent != NULL);
    KASSERT(child != NULL);

    pft = parent->proc_ft;

    /* If parent has no file table, child also has none. */
    if (pft == NULL) {
        child->proc_ft = NULL;
        return 0;
    }

    cft = kmalloc(sizeof(struct ft));
    if (cft == NULL) {
        return ENOMEM;
    }

    cft->ft_lock = lock_create("ft_lock_child");
    if (cft->ft_lock == NULL) {
        kfree(cft);
        return ENOMEM;
    }

    /* Start with all entries empty. */
    for (int i = 0; i < OPEN_MAX; i++) {
        cft->entries[i] = NULL;
    }

    /* Copy pointers and bump refcnts. */
    lock_acquire(pft->ft_lock);
    for (int i = 0; i < OPEN_MAX; i++) {
        struct filetable *ftable = pft->entries[i];
        if (ftable == NULL) {
            continue;
        }

        cft->entries[i] = ftable;

        /* Increase refcnt on the shared file object. */
        lock_acquire(ftable->lock);
        ftable->refcnt++;
        lock_release(ftable->lock);
    }

    lock_release(pft->ft_lock);

    child->proc_ft = cft;
    return 0;
}
