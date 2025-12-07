#include <types.h>
#include <lib.h>
#include <kern/errno.h>
#include <addrspace.h>
#include <current.h>
#include <vm.h>
#include <machine/tlb.h>
#include <machine/vm.h>
#include <spl.h>
#include <proc.h>
#include <spinlock.h>

#define VM_STACKPAGES 64  /* 64 * 4 = 256 kb*/

/* Temporary: still use ram_stealmem, guarded by a lock */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

struct coremap_entry *coremap;
unsigned long coremap_npages;
paddr_t coremap_firstpaddr;
bool coremap_ready = false;
struct spinlock coremap_lock = SPINLOCK_INITIALIZER;

void vm_bootstrap(void)
{
    paddr_t first, last;
    last = ram_getsize();
    first = ram_getfirstfree();

    unsigned long total_frames = (last - first) / PAGE_SIZE;

    size_t coremap_bytes = total_frames * sizeof(struct coremap_entry);
    size_t coremap_pages = (coremap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Put the coremap at the beginning of RAM */
    paddr_t coremap_pa = first;
    first += coremap_pages * PAGE_SIZE;   /* Skip space used by coremap */

    coremap_npages     = (last - first) / PAGE_SIZE;
    coremap_firstpaddr = first;

    /* Map the coremap array into kernel virtual memory */
    coremap = (struct coremap_entry *)PADDR_TO_KVADDR(coremap_pa);

    /* Initialize entries */
    for (unsigned long i = 0; i < coremap_npages; i++) {
        coremap[i].pa        = first + i * PAGE_SIZE;
        coremap[i].free      = true;
        coremap[i].kernel    = false;
        coremap[i].as        = NULL;
        coremap[i].vaddr     = 0;
        coremap[i].chunk_len = 0;
    }

    coremap_ready = true;
}

vaddr_t alloc_kpages(unsigned npages)
{
    if (!coremap_ready) {
        paddr_t pa;

        spinlock_acquire(&stealmem_lock);
        pa = ram_stealmem(npages);
        spinlock_release(&stealmem_lock);

        if (pa == 0) {
            return 0;
        }
        return PADDR_TO_KVADDR(pa);
    }

    spinlock_acquire(&coremap_lock);

    unsigned long run_start = 0;
    unsigned long run_len   = 0;
    bool found              = false;

    for (unsigned long i = 0; i < coremap_npages; i++) {
        if (coremap[i].free) {
            if (run_len == 0) {
                run_start = i;
            }
            run_len++;
            if (run_len == npages) {
                found = true;
                break;
            }
        } else {
            run_len = 0;
        }
    }

    if (!found) {
        spinlock_release(&coremap_lock);
        return 0; 
    }

    /* Mark pages allocated */
    for (unsigned long i = run_start; i < run_start + npages; i++) {
        coremap[i].free      = false;
        coremap[i].kernel    = true;
        coremap[i].as        = NULL;
        coremap[i].vaddr     = 0;
        coremap[i].chunk_len = 0;
    }
    /* only the first page knows the size of the chunk */
    coremap[run_start].chunk_len = npages;

    paddr_t pa = coremap[run_start].pa;

    spinlock_release(&coremap_lock);

    return PADDR_TO_KVADDR(pa);
}

void
free_kpages(vaddr_t addr)
{
    if (!coremap_ready) {
        /* Early-boot allocations from ram_stealmem can't be safely freed */
        (void)addr;
        return;
    }

    paddr_t pa = (paddr_t)(addr - MIPS_KSEG0);

    spinlock_acquire(&coremap_lock);

    /* Check address is within managed range */
    if (pa < coremap_firstpaddr) {
        spinlock_release(&coremap_lock);
        return;
    }

    unsigned long idx = (pa - coremap_firstpaddr) / PAGE_SIZE;
    if (idx >= coremap_npages) {
        spinlock_release(&coremap_lock);
        return;
    }

    unsigned npages = coremap[idx].chunk_len;
    if (npages == 0) {
        /* Either not the start of a chunk or already freed */
        spinlock_release(&coremap_lock);
        return;
    }

    for (unsigned long i = idx; i < idx + npages && i < coremap_npages; i++) {
        coremap[i].free      = true;
        coremap[i].kernel    = false;
        coremap[i].as        = NULL;
        coremap[i].vaddr     = 0;
        coremap[i].chunk_len = 0;
    }

    spinlock_release(&coremap_lock);
}

void
vm_tlbshootdown_all(void)
{
    /* No-op */
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
    /* No-op */
    (void)ts;
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
    vaddr_t faultpage = faultaddress & PAGE_FRAME;
    struct addrspace *as;
    paddr_t paddr = 0;

    /* Fault type */
    switch (faulttype) {
        case VM_FAULT_READ:
        case VM_FAULT_WRITE:
        case VM_FAULT_READONLY:
            break;
        default:
            return EINVAL;
    }

    if (curproc == NULL) {
        return EFAULT;
    }

    as = proc_getas();
    if (as == NULL) {
        return EFAULT;
    }

    /* Reuse a page if already allocated to this as and vaddr or allcoate new one */
    spinlock_acquire(&coremap_lock);
    for (unsigned long i = 0; i < coremap_npages; i++) {
        if (!coremap[i].free &&
            !coremap[i].kernel &&
            coremap[i].as == as &&
            coremap[i].vaddr == faultpage) {
            paddr = coremap[i].pa;
            break;
        }
    }

    if (paddr == 0) {
        unsigned long idx;
        for (idx = 0; idx < coremap_npages; idx++) {
            if (coremap[idx].free) {
                coremap[idx].free      = false;
                coremap[idx].kernel    = false;
                coremap[idx].as        = as;
                coremap[idx].vaddr     = faultpage;
                coremap[idx].chunk_len = 1;
                paddr = coremap[idx].pa;
                break;
            }
        }

        if (paddr == 0) {
            spinlock_release(&coremap_lock);
            return ENOMEM;
        }

        bzero((void *)PADDR_TO_KVADDR(paddr), PAGE_SIZE);
    }

    spinlock_release(&coremap_lock);

    /* Map it into the TLB */
    uint32_t ehi = faultpage;
    uint32_t elo = paddr | TLBLO_VALID | TLBLO_DIRTY;

    int spl = splhigh();

    /* Try a free slot first */
    for (int i = 0; i < NUM_TLB; i++) {
        uint32_t oldhi, oldlo;
        tlb_read(&oldhi, &oldlo, i);
        if (!(oldlo & TLBLO_VALID)) {
            tlb_write(ehi, elo, i);
            splx(spl);
            return 0;
        }
    }

    /* No free slot, evict a random one */
    int victim = random() % NUM_TLB;
    tlb_write(ehi, elo, victim);

    splx(spl);
    return 0;
}

void vm_free_as(struct addrspace *as)
{
    if (!coremap_ready || as == NULL) {
        return;
    }

    spinlock_acquire(&coremap_lock);

    for (unsigned long i = 0; i < coremap_npages; i++) {
        if (!coremap[i].free && coremap[i].as == as && !coremap[i].kernel) {
            coremap[i].free      = true;
            coremap[i].as        = NULL;
            coremap[i].vaddr     = 0;
            coremap[i].chunk_len = 0;
        }
    }

    spinlock_release(&coremap_lock);
}
