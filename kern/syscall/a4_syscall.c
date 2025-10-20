#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <vfs.h>
#include <vnode.h>
#include <copyinout.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <limits.h>
#include <kern/fcntl.h>
// #include <filetable.h>




int
sys_open(const_userptr_t filename, int flags, int *retval){
	
	char kfilename[PATH_MAX];
    	size_t actual;
    	int result;

    	if (filename == NULL) {
        	return EFAULT;
    	}

    	// Copy filename safely from userspace
    	result = copyinstr(filename, kfilename, sizeof(kfilename), &actual);
    	if (result) {
    	    return result;
    	}

    	// Validate flags
    	if ((flags & O_ACCMODE) != O_RDONLY &&
        	(flags & O_ACCMODE) != O_WRONLY &&
        	(flags & O_ACCMODE) != O_RDWR) {
        	return EINVAL;
    	}

    	// (Later) call vfs_open, filetable_place, etc.
    	kprintf("Opening file: %s\n", kfilename);

    	*retval = 3; // fake file descriptor (0,1,2 reserved)
    	return 0;


}
