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
#include <kern/seek.h>
#include "filetable.h"

static struct filetable *file_get(int fd) {
    if (fd < 0 || fd >= OPEN_MAX) return NULL;
    return curproc->ft[fd];
}

static int file_allocfd(void) {
    for (int i = 0; i < OPEN_MAX; i++) {
        if (curproc->ft[i] == NULL)
            return i;
    }
    return -1;
}

/*
 * Open a file
 */
int sys_open(const_userptr_t filename, int flags, mode_t mode, int *retval) {
    (void)mode;
    char kfilename[PATH_MAX];
    int result = copyinstr(filename, kfilename, sizeof(kfilename), NULL);
    if (result) return result;

    struct vnode *vn;
    result = vfs_open(kfilename, flags, 0, &vn);
    if (result) return result;

    struct filetable *of = kmalloc(sizeof(struct filetable));
    if (of == NULL) {
        vfs_close(vn);
        return ENOMEM;
    }

    of->vn = vn;
    of->offset = 0;
    of->flags = flags;
    of->refcnt = 1;
    of->lock = lock_create("of_lock");
    if (of->lock == NULL) {
        kfree(of);
        vfs_close(vn);
        return ENOMEM;
    }

    int fd = file_allocfd();
    if (fd < 0) {
        lock_destroy(of->lock);
        vfs_close(vn);
        kfree(of);
        return EMFILE;
    }

    curproc->ft[fd] = of;
    *retval = fd;
    return 0;
}

/*
 * Read a file
 */
int sys_read(int fd, userptr_t buf, size_t buflen, int32_t *retval) {
    struct filetable *of = file_get(fd);
    if (of == NULL) return EBADF;
    if ((of->flags & O_ACCMODE) == O_WRONLY) return EBADF;

    struct iovec iov;
    struct uio uio;
    lock_acquire(of->lock);
    uio_kinit(&iov, &uio, buf, buflen, of->offset, UIO_READ);

    int result = VOP_READ(of->vn, &uio);
    if (result) {
        lock_release(of->lock);
        return result;
    }

    of->offset = uio.uio_offset;
    *retval = buflen - uio.uio_resid;
    lock_release(of->lock);
    return 0;
}

int sys_close(int fd) {
    struct filetable *of = file_get(fd);
    if (of == NULL) return EBADF;

    lock_acquire(of->lock);
    of->refcnt--;
    if (of->refcnt == 0) {
        lock_release(of->lock);
        vfs_close(of->vn);
        lock_destroy(of->lock);
        kfree(of);
    } else {
        lock_release(of->lock);
    }

    curproc->ft[fd] = NULL;
    return 0;
}

int sys_dup2(int oldfd, int newfd, int32_t *retval) {
    if (oldfd < 0 || oldfd >= OPEN_MAX || newfd < 0 || newfd >= OPEN_MAX)
        return EBADF;

    struct filetable *oldf = file_get(oldfd);
    if (oldf == NULL) return EBADF;

    if (oldfd == newfd) {
        *retval = newfd;
        return 0;
    }

    if (curproc->ft[newfd] != NULL)
        sys_close(newfd);

    lock_acquire(oldf->lock);
    oldf->refcnt++;
    lock_release(oldf->lock);

    curproc->ft[newfd] = oldf;
    *retval = newfd;
    return 0;
}

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
 * Check current position in file
 */
int sys_lseek(int fd, off_t pos, int whence, int32_t *retval, int32_t *retval2)
{
	if (fd < 0 || fd >= OPEN_MAX) {
		return EBADF;
	}

	struct filetable *ftable = curproc->ft[fd];
	if (ftable == NULL) {
		return EBADF;
	}

	if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
		return EINVAL;
	}

	if (!VOP_ISSEEKABLE(ftable->vn)) {
		return ESPIPE;
	}

	// New offset
	int err;
	struct stat st;
	off_t newoff;

	lock_acquire(ftable->lock);

	switch (whence) {
	case SEEK_SET:
		newoff = pos;
		break;
	case SEEK_CUR:
		newoff = ftable->offset + pos;
		break;
	case SEEK_END:
		err = VOP_STAT(ftable->vn, &st);
        	if (err) {
            		lock_release(ftable->lock);
            		return err;
        	}
        	newoff = st.st_size + pos;
        	break;
	default:
		lock_release(ftable->lock);
		return EINVAL;  // Wrong case, never happned
	}

	if (newoff < 0) {
		lock_release(ftable->lock);
		return EINVAL;
	}

	ftable->offset = newoff;
	lock_release(ftable->lock);

	// Split in 32 bit
	*retval = (int32_t)((uint64_t)newoff >> 32);
    	*retval2 = (int32_t)((uint64_t)newoff & 0xffffffffu);

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
