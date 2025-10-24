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

/*
 * file_get returns filetable entry with file descriptor (fd).
 * It returns NULL if fd is invalid or not opened.
 */
static struct filetable *file_get(int fd) {
    if (fd < 0 || fd >= OPEN_MAX) return NULL;
    return curproc->ft[fd];
}

/*
 * file_allocfd finds the first available file descriptor position. 
 * It returns the index of fd, or -1 if there's no space. 
 * */
static int file_allocfd(void) {
    for (int i = 0; i < OPEN_MAX; i++) {
        if (curproc->ft[i] == NULL)
            return i;
    }
    return -1;
}

/*
 * open system call.
 * Opens a file, device, or other kernel object named by the pathname filename from user space, 
 * and then returns a new file descriptor. 
 */
int sys_open(const_userptr_t filename, int flags, mode_t mode, int *retval) {
    	(void)mode;

    	/* filename is an invalid pointer */
    	if (filename == NULL) {
    		return EFAULT;
    	}

    	/* copy filename from user space into kernel buffer */
    	char kfilename[PATH_MAX];
    	int result = copyinstr(filename, kfilename, sizeof(kfilename), NULL);
    	if (result) return result;

    	/* empty path string is invalid values */
    	if (kfilename[0] == '\0' ) {
    		return EINVAL;
    	}

    	/* validate access mode */
    	int accmode = flags & O_ACCMODE;
    	if (accmode != O_RDONLY && accmode != O_WRONLY && accmode != O_RDWR) {
    		return EINVAL;
    	}

    	/* only allow these seven flags */
    	int allowed_flags = O_RDONLY | O_WRONLY | O_RDWR | O_CREAT | O_EXCL | O_TRUNC | O_APPEND;
    	if (flags & ~allowed_flags) {
        	return EINVAL;
    	}


	/* open file via vfs */
    	struct vnode *vn;
    	result = vfs_open(kfilename, flags, 0, &vn);
    	if (result) return result;

	/* allocate filetable. If there's no space for filetable, then return no memory error. */
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

	/* allocate file descripter */
    	int fd = file_allocfd();

	/* error when too many open files */
    	if (fd < 0) {
        	lock_destroy(of->lock);
        	vfs_close(vn);
        	kfree(of);
        	return EMFILE;
   	}
	
	/* save filetable and return file descripter */
    	curproc->ft[fd] = of;
    	*retval = fd;
    	return 0;
}

/*
 * read system call.
 * Read up to buflen bytes from the file specified by fd, at the location in the file specified 
 * by the current seek position of the file, and stores them in the space pointed to by buf.
 */
int sys_read(int fd, userptr_t buf, size_t buflen, int32_t *retval) {
    	struct filetable *of = file_get(fd);
	
	/* fd is not a valid file descriptor, or was not opened for reading. */
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

	/* Update offset and return number of bytes read */
    	of->offset = uio.uio_offset;
    	*retval = buflen - uio.uio_resid;
    	lock_release(of->lock);
    	return 0;
}

/*
 * Close system call.
 * Closes file descriptor. 
 */
int sys_close(int fd) {
    	struct filetable *of = file_get(fd);
	/* 	file descriptor is not a valid file handle. */
    	if (of == NULL) return EBADF;

    	lock_acquire(of->lock);


	/* decrement reference counter */
    	of->refcnt--;
	 /* if it's last reference, then clean up resources */
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

/*
 * Dup2 system call.
 * clones the file handle oldfd onto the file handle newfd.
 */
int sys_dup2(int oldfd, int newfd, int32_t *retval) {
	/* oldfd is not a valid file handle, or newfd is a value that cannot be a valid file handle. */
    	if (oldfd < 0 || oldfd >= OPEN_MAX || newfd < 0 || newfd >= OPEN_MAX)
       		return EBADF;

    	struct filetable *oldf = file_get(oldfd);
	/* oldfd is not a valid file handle. */
    	if (oldf == NULL) return EBADF;

	/* when they are the same, nothing to do */
    	if (oldfd == newfd) {
        	*retval = newfd;
        	return 0;
    	}
	/* close newfd if it is opened */
    	if (curproc->ft[newfd] != NULL)
        	sys_close(newfd);

	/* increment reference counter in filetable */
   	lock_acquire(oldf->lock);
   	oldf->refcnt++;
    	lock_release(oldf->lock);

    	curproc->ft[newfd] = oldf;
    	*retval = newfd;
    	return 0;
}

/*
 * Write system call.
 * writes up to buflen bytes to the file specified by fd, at the location in the file specified 
 * by the current seek position of the file, taking the data from the space pointed to by buf.
 */
int sys_write(int fd, const void *buf, size_t nbytes, int32_t *retval)
{
	/* fd is not a valid file descriptor, or was not opened for writing. */
	if (fd < 0 || fd >= OPEN_MAX) {
		return EBADF;
	}

	/* Part or all of the address space pointed to by buf is invalid. */
	if (curproc == NULL) {
		return EFAULT;
	}

	struct filetable *ftable = curproc->ft[fd];
	/* fd is not opened */
	if (ftable == NULL) {
		return EBADF;  // Not open
	}

	/* must be open for writing */
	if ((ftable->flags & (O_WRONLY | O_RDWR)) == 0) {
		return EBADF;
	}

	lock_acquire(ftable->lock);

	struct iovec iov;
	struct uio u;

	/* initialize uio for user space write */
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

	/* update offset and return written bytes */
	size_t wrote = nbytes - u.uio_resid;
	ftable->offset += (off_t)wrote;

	lock_release(ftable->lock);

	*retval = (int32_t)wrote;
	return 0;
}

/*
 * lseek system call.
 * alters the current seek position of the file handle filehandle, seeking to a new position based on pos and whence.
 */
int sys_lseek(int fd, off_t pos, int whence, int32_t *retval, int32_t *retval2)
{
	/* fd is not a valid file handle. */
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

	/* Invlid seek */
	if (!VOP_ISSEEKABLE(ftable->vn)) {
		return ESPIPE;
	}

	/* compute new offset */
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
		return EINVAL;  
	}

	if (newoff < 0) {
		lock_release(ftable->lock);
		return EINVAL;
	}

	ftable->offset = newoff;
	lock_release(ftable->lock);

	/* Split offset into 32 bit */
	*retval = (int32_t)((uint64_t)newoff >> 32);
    	*retval2 = (int32_t)((uint64_t)newoff & 0xffffffffu);

	return 0;
}

/*
 * chdir system call.
 * Change current working directory to user path.
 */
int sys_chdir(const char *upath)
{
	/* pathname was an invalid pointer. */
	if (upath == NULL) {
		return EFAULT;
	}

	char *kpath = kmalloc(PATH_MAX);
	/* when allocation is failed, no memory error */
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
 * __getcwd system call. 
 * Copies the current working directory path into user buffer.
 */

int sys___getcwd(char *buf, size_t buflen, int32_t *retval)
{
    	if (buflen == 0) {
        	return EINVAL;
    	}

    	struct iovec iov;
    	struct uio u;
    	int result;

    	iov.iov_ubase = (userptr_t)buf;  
    	iov.iov_len   = buflen;

	/* Initialize uio for reading from kernel into user buffer */
    	u.uio_iov     = &iov;
    	u.uio_iovcnt  = 1;
	/* bytes left */
    	u.uio_resid   = buflen;  
	/* not used by getcwd */
    	u.uio_offset  = 0;  
    	u.uio_segflg  = UIO_USERSPACE;
    	u.uio_rw      = UIO_READ;
    	u.uio_space   = curproc->p_addrspace;

    	result = vfs_getcwd(&u);
    	if (result) {
        	return result;
    	}

    	/* return bytes successfully copied */
    	*retval = buflen - u.uio_resid;
    	return 0;
}
