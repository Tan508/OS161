#include <types.h>
#include <vfs.h>
#include <current.h>
#include <syscall.h>
#include <proc.h>
#include <uio.h>
#include <vnode.h>
#include <kern/stat.h>
#include <kern/fcntl.h>
#include <kern/errno.h>
#include <vm.h>
#include <copyinout.h>
#include <limits.h>
#include "filetable.h"

/*
 * Write data to file
 */
int sys_write(int fd, const void *buf, size_t nbytes, int32_t *retval)
{
	if (fd < 0 || fd >= OPEN_MAX) {
		return EBADF;
	}

	if (curproc == NULL) {
		return EFAULT;
	}

	struct filetable *ftable = curproc->ft[fd];
	if (ftable == NULL) {
		return EBADF;  // Not open
	}

	// Open for writing
	if ((ftable->flags & (O_WRONLY | O_RDWR)) == 0) {
		return EBADF;
	}

	lock_acquire(ftable->lock);

	struct iovec iov;
	struct uio u;

	iov.iov_ubase = (userptr_t)buf;
  	iov.iov_len = nbytes;

    	u.uio_iov =  &iov;
    	u.uio_iovcnt = 1;
    	u.uio_resid = nbytes;
    	u.uio_offset = ftable->offset;
    	u.uio_segflg = UIO_USERSPACE;
    	u.uio_rw = UIO_WRITE;
    	u.uio_space = curproc->p_addrspace;

	int result = VOP_WRITE(ftable->vn, &u);
	if (result) {
		lock_release(ftable->lock);
		return result;
	}

	size_t wrote = nbytes - u.uio_resid;
	ftable->offset += (off_t)wrote;

	lock_release(ftable->lock);

	*retval = (int32_t)wrote;
	return 0;
}


/*
 * Change the directory to user path
 */
int sys_chdir(const char *upath)
{
	if (upath == NULL) {
		return EFAULT;
	}

	char *kpath = kmalloc(PATH_MAX);
	if (kpath == NULL) {
		return ENOMEM;
	}

	size_t *klength = kmalloc(sizeof(int));

	int err = copyinstr((const_userptr_t)upath, kpath, PATH_MAX, klength);
	if (err) {
		kfree(kpath);
		kfree(klength);
		return err;
	}

	int result= vfs_chdir((char *)upath);
	kfree(kpath);
	kfree(klength);
	if (result) {
		return result;
	}

	return err;
}

/*
 * Get current working direcotry
 */
int sys___getcwd(char *buf, size_t buflen, int32_t *retval)
{
    	if (buflen == 0) {
        	return EINVAL;
    	}

    	struct iovec iov;
    	struct uio u;
    	int result;

    	iov.iov_ubase = (userptr_t)buf;  // user buffer
    	iov.iov_len   = buflen;

    	u.uio_iov     = &iov;
    	u.uio_iovcnt  = 1;
    	u.uio_resid   = buflen;  // bytes left
    	u.uio_offset  = 0;  // not used by getcwd
    	u.uio_segflg  = UIO_USERSPACE;
    	u.uio_rw      = UIO_READ;
    	u.uio_space   = curproc->p_addrspace;

    	result = vfs_getcwd(&u);
    	if (result) {
        	return result;
    	}

    	// Bytes successfully copied
    	*retval = buflen - u.uio_resid;
    	return 0;
}
