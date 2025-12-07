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

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <spl.h>
#include <spinlock.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}


	/*
	 * Initialize
	 */
	as->regions = NULL;
	as->heap_start = 0;
	as->heap_end = 0;

	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;

	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}

	/* Copy regions*/
	newas->regions = NULL;
	struct region **tail = &newas->regions;
	for (struct region *curr = old->regions; curr != NULL; curr = curr->next) {
		struct region *new_region = kmalloc(sizeof(struct region));
		if (new_region == NULL) {
			as_destroy(newas);
			return ENOMEM;
		}

		new_region->vbase = curr->vbase;
		new_region->npages = curr->npages;
		new_region->perms = curr->perms;
		new_region->next = NULL;

		*tail = new_region;
		tail = &new_region->next;
	}

	/* Copy heap info */
	newas->heap_start = old->heap_start;
	newas->heap_end = old->heap_end;

	/* Copy physicall pages belong to old by scanning the coremap */
	spinlock_acquire(&coremap_lock);

	for (unsigned long i = 0; i < coremap_npages; i++) {
		if (!coremap[i].free && coremap[i].as == old && !coremap[i].kernel) {
			vaddr_t vaddr = coremap[i].vaddr;
			paddr_t paddr = coremap[i].pa;

			/* Find free cormap slot */
			unsigned long j;
			for (j = 0; j < coremap_npages; j++) {
				if (coremap[j].free) {
					coremap[j].free      = false;
					coremap[j].kernel    = false;
					coremap[j].as        = newas;
					coremap[j].vaddr     = vaddr;
					coremap[j].chunk_len = 1;
					break;
				}
			}

			if (j == coremap_npages) {
				/* No free slot */
				spinlock_release(&coremap_lock);
				as_destroy(newas);
				return ENOMEM;
			}

			paddr_t pa_new = coremap[j].pa;
			/* Copy the content */
			memmove((void *)PADDR_TO_KVADDR(pa_new),
			        (const void *)PADDR_TO_KVADDR(paddr),
					PAGE_SIZE);
		}
	}
	
	spinlock_release(&coremap_lock);

	*ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	if (as == NULL) {
		return;
	}

	/* Free all physical pages belong to this as */
	vm_free_as(as);

	struct region *curr = as->regions;
	struct region *next;
	while (curr != NULL) {
		next = curr->next;
		kfree(curr);
		curr = next;
	}

	kfree(as);
}

void
as_activate(void)
{
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/* Kernel thread without an address space */
		return;
	}

	(void)as;

	/* Flush the TLB */
	int spl = splhigh();
	for (int i = 0; i < NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
	splx(spl);
}

void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	/* Align base*/
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* Align length */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;
	size_t npages = sz / PAGE_SIZE;

	struct region *new_region = kmalloc(sizeof(struct region));
	if (new_region == NULL) {
		return ENOMEM;
	}

	new_region->vbase = vaddr;
	new_region->npages = npages;

	int perms = 0;
	if (readable) {
		perms |= 0x1;
	}
	if (writeable) {
		perms |= 0x2;
	}
	if (executable) {
		perms |= 0x4;
	}

	new_region->perms = perms;
	new_region->next = as->regions;
	as->regions = new_region;

	return 0;
}

int
as_prepare_load(struct addrspace *as)
{
	(void)as;
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	/* Find the highest end address among all regions */
	vaddr_t max_end = 0;

	for (struct region *curr = as->regions; curr != NULL; curr = curr->next) {
		vaddr_t region_end = curr->vbase + curr->npages * PAGE_SIZE;
		if (region_end > max_end) {
			max_end = region_end;
		}
	}

	/* Initilize heap */
	as->heap_start = max_end;
	as->heap_end = max_end;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	/*
	 * Write this.
	 */

	(void)as;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}

