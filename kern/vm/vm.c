#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <proc.h>
#include <mips/tlb.h>
#include <current.h>
#include <spl.h>
#include <synch.h>


/* Place your page table functions here */

int
page_table_insert(vaddr_t hi, paddr_t lo, struct addrspace *as)
{


    uint32_t ind;
    ind = (((uint32_t )as) ^ (hi >> 12)) % hpt_size;
    struct HPT *curr;

    /* first entry found is empty */
    if (page_table[ind].lo == 0) {
        page_table[ind].hi = hi;
        page_table[ind].lo = lo;
        page_table[ind].as = as;
        page_table[ind].next = NULL;
        return 0;

        /* first entry is not empty but no chained entries */
    } else if (page_table[ind].next == NULL) {

        curr = kmalloc(sizeof(struct HPT *));

        if (curr == NULL) {
        /* Return no memory error ENOMOM */
            return ENOMEM;
        }

        page_table[ind].next = curr;

        curr->hi = hi;
        curr->lo = lo;
        curr->as = as;
        curr->next = NULL;
        return 0;
    }

    // Finding last external chain entry.
    curr = page_table[ind].next;

    while (curr->next != NULL) {
        curr = curr->next;
    }

    curr->next = kmalloc(sizeof(struct HPT *));

    if (curr->next == NULL) {
        /* Return no memory error ENOMOM */
        return ENOMEM;
    }

    // assign page table entry.
    curr = curr->next;
    curr->hi = hi;
    curr->lo = lo;
    curr->as = as;
    curr->next = NULL;

    return 0;

}

void vm_bootstrap(void)
{
    /* Initialise any global components of your VM sub-system here.  
     *
     * You may or may not need to add anything here depending what's
     * provided or required by the assignment spec.
     */

    uint32_t size;
    page_table_lock = lock_create("page_table_lock");
    
    size = ram_getsize();
    size = size / PAGE_SIZE;
    hpt_size = size;

    /* Initialising hashed page table. */ 
    page_table = kmalloc(hpt_size * sizeof(struct HPT));

    if (page_table == NULL) {
        return;
    }

    /* initialising all entries to invalid hi/lo */
    lock_acquire(page_table_lock);
    for (uint32_t count = 0; count < hpt_size; count++) {
        page_table[count].hi = TLBHI_INVALID(count % 64);
        page_table[count].lo = TLBLO_INVALID();
        page_table[count].as = NULL;
        page_table[count].next = NULL;
    }
    lock_release(page_table_lock);

}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{

    /* fault address is just the vpn */ 
    faultaddress &= PAGE_FRAME;
    
    /* Check that faultaddress is within valid range. */ 
    if (faultaddress == 0) {
        return EFAULT;
    } else if (faultaddress >= USERSTACK) {
        return EINVAL;
    }

    struct addrspace *as;

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
            return EFAULT;
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		    break;
	    default:
		    return EINVAL;
	}

    if (curproc == NULL) {
	/*
	 * No process. This is probably a kernel fault early
	 * in boot. Return EFAULT so as to panic instead of
	 * getting into an infinite faulting loop.
	 */
	    return EFAULT;
	}

	as = proc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

    /**
     * Checking if fault address is within curr_procs adress space.
     */

    struct mem_region *curr = as->head;
    uint32_t perms = 0;

    while (curr != NULL) {
        vaddr_t base, top;

        base = curr->vbase & PAGE_FRAME;
        top = curr->vbase + (curr->size * PAGE_SIZE);

        /* If address is insde range, grab perms. */ 
        if (base <= faultaddress && top > faultaddress) {
            perms = curr->mode;
            break;
        }

        curr = curr->next;
    }

    /* faultaddress was not within any of the memory regions */
    if (curr == NULL) {
        return EFAULT;
    }

    uint32_t hi_ind;

    hi_ind = (((uint32_t )as) ^ (faultaddress >> 12)) % hpt_size;
    int spl;

    /* checking if region has write permissions (PF_W) */ 
    if ((perms & WRITE_MODE) == WRITE_MODE) {
        perms = TLBLO_DIRTY;
    } else {
        perms = 0;
    }

    /* Adding valid ctrl bit */ 
    perms |= TLBLO_VALID;

    /** Finding page table entry. **/ 

    /* if faultaddres is the first in list. */ 
    lock_acquire(page_table_lock);
    if (page_table[hi_ind].hi == faultaddress && page_table[hi_ind].lo != 0) {

        spl = splhigh();
        tlb_random(page_table[hi_ind].hi, page_table[hi_ind].lo | perms);
        splx(spl);

        lock_release(page_table_lock);
        return 0;

        /* checking for fault address within the collision list. */ 
    } else if (page_table[hi_ind].lo != 0 && page_table[hi_ind].next != NULL) {
        struct HPT *curr_pte = page_table[hi_ind].next;

        /* traversing through collision list for corresponding entryhi */ 
        while (curr_pte != NULL) {
            if (curr_pte->hi == faultaddress && curr_pte->lo != 0) {

                spl = splhigh();
                tlb_random(curr_pte->hi, curr_pte->lo | perms);
                splx(spl);

                lock_release(page_table_lock);
                return 0;
            }
            curr_pte = curr_pte->next;
        }
    } 
    
    vaddr_t page_addr;

    page_addr = alloc_kpages(ONE_PAGE);

    if (page_addr == 0) {
        lock_release(page_table_lock);
        return ENOMEM;
    }

    /* change from kernel space to user space */
    page_addr = KVADDR_TO_PADDR(page_addr);

    /* zero fill the frame */
    bzero((void *)PADDR_TO_KVADDR(page_addr), 1 * PAGE_SIZE);

    int ret;

    /* Inserting newly malloced page into page table. */ 
    ret = page_table_insert(faultaddress, page_addr | perms, as);

    if (ret) {
        free_kpages(PADDR_TO_KVADDR(page_addr));
        lock_release(page_table_lock);
        return EFAULT;
    }

    /* disabling interrupts for tlb_random() */ 
    spl = splhigh();
    tlb_random(faultaddress, page_addr | perms);
    splx(spl);
    lock_release(page_table_lock);

    return 0;
}

/*
 * SMP-specific functions.  Unused in our UNSW configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

