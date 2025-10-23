/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Sample/test code for running a user program.  You can use this for
 * reference when implementing the execv() system call. Remember though
 * that execv() needs to do more than runprogram() does.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
#include "filetable.h"
#include <vnode.h>
#include <synch.h>
#include <limits.h>

static struct filetable *
ft_entry_create(struct vnode *vn, int flags)
{
    struct filetable *e = kmalloc(sizeof(*e));
    if (!e) return NULL;
    e->refcnt = 1;
    e->vn     = vn;
    e->offset = 0;
    e->flags  = flags;
    e->lock   = lock_create("ft_entry");
    if (!e->lock) { kfree(e); return NULL; }
    return e;
}

static int
ft_install_at(struct filetable **ft, int want_fd, struct filetable *e)
{
    if (want_fd < 0 || want_fd >= OPEN_MAX) return EMFILE;
    if (ft[want_fd] != NULL) return EBUSY;
    ft[want_fd] = e;
    return 0;
}

#include <lib.h>     // kstrdup, kfree
#include <vfs.h>
#include <vnode.h>
#include <kern/fcntl.h>

static int
setup_std_fds(void)
{
    int err;
    struct vnode *vn;
    struct filetable *e;

    /* fd 0: stdin */
    char *con0 = kstrdup("con:");
    if (!con0) return ENOMEM;
    err = vfs_open(con0, O_RDONLY, 0, &vn);
    kfree(con0);
    if (err) return err;
    e = ft_entry_create(vn, O_RDONLY);
    if (!e) { vfs_close(vn); return ENOMEM; }
    err = ft_install_at(curproc->ft, 0, e);
    if (err) { vfs_close(vn); lock_destroy(e->lock); kfree(e); return err; }

    /* fd 1: stdout */
    char *con1 = kstrdup("con:");
    if (!con1) return ENOMEM;
    err = vfs_open(con1, O_WRONLY, 0, &vn);
    kfree(con1);
    if (err) return err;
    e = ft_entry_create(vn, O_WRONLY);
    if (!e) { vfs_close(vn); return ENOMEM; }
    err = ft_install_at(curproc->ft, 1, e);
    if (err) { vfs_close(vn); lock_destroy(e->lock); kfree(e); return err; }

    /* fd 2: stderr */
    char *con2 = kstrdup("con:");
    if (!con2) return ENOMEM;
    err = vfs_open(con2, O_WRONLY, 0, &vn);
    kfree(con2);
    if (err) return err;
    e = ft_entry_create(vn, O_WRONLY);
    if (!e) { vfs_close(vn); return ENOMEM; }
    err = ft_install_at(curproc->ft, 2, e);
    if (err) { vfs_close(vn); lock_destroy(e->lock); kfree(e); return err; }

    return 0;
}



int
runprogram(char *progname)
{
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	char *kprog = kstrdup(progname); //make a writable copy
        if (kprog == NULL) return ENOMEM;

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	kfree(kprog);
	if (result) {
		return result;
	}

	/* We should be a new process. */
	KASSERT(proc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	proc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

	//
	result = setup_std_fds();
	if (result) return result;
	/* Warp to user mode. */
	enter_new_process(0 /*argc*/, NULL /*userspace addr of argv*/,
			  NULL /*userspace addr of environment*/,
			  stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

