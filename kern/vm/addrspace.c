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
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <elf.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

/* creates address space */
struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	as->head = NULL;

	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{

	KASSERT(old != NULL);

	struct mem_region *new_region, *old_region;
	struct addrspace *newas = as_create();

	old_region = old->head;
	new_region = NULL;
	
	while (old_region != NULL) {

		struct mem_region *temp;

		/* add arguments to init. */ 
		temp = init_region(old_region->vbase, 
						   old_region->size, 
						   old_region->mode, 
						   old_region->acc_mode);

		if (temp == NULL) {
			/* delete all pages before it */
			as_destroy(newas);
			lock_release(page_table_lock);
			return ENOMEM;
		}

		/* adding to end of new list */
		if (newas->head == NULL) {
			newas->head = temp;
			new_region = temp;
		} else {
			new_region->next = temp;
			new_region = new_region->next;
		}

		if (old_region->next == NULL) {
			break;
		}
		old_region = old_region->next;
	}

	*ret = newas;

	/** mallocing new pte to the table **/

	lock_acquire(page_table_lock);
	for (uint32_t i = 0; i < hpt_size; i++) {
		
		/* if no entry, skip */
    	if (page_table[i].lo == 0 || page_table[i].as == NULL) {
			continue;
		}
		
		/* first entry corresponding to old addrspace */
		if (page_table[i].as == old) {
			vaddr_t page_addr = alloc_kpages(1);
			page_addr = KVADDR_TO_PADDR(page_addr);

			if (page_addr == 0) {
				lock_release(page_table_lock);
				return ENOMEM;
			}

			memmove((void *)PADDR_TO_KVADDR(page_addr), 
				(const void *)PADDR_TO_KVADDR(page_table[i].lo & PAGE_FRAME), 
				PAGE_SIZE);

			vaddr_t perms = (page_table[i].lo & CTRL_BIT_MASK) | TLBLO_VALID;
			page_table_insert(page_table[i].hi & PAGE_FRAME, page_addr | perms, newas);
		}

		struct HPT *curr = page_table[i].next;

		/* Checking rest of collision list for old addrspace */
		while (curr != NULL) {
			if (curr->as == old) {
				vaddr_t page_addr = alloc_kpages(1);
				page_addr = KVADDR_TO_PADDR(page_addr);

				if (page_addr == 0) {
					lock_release(page_table_lock);
					return ENOMEM;
				}

				memmove((void *)PADDR_TO_KVADDR(page_addr), 
					(const void *)PADDR_TO_KVADDR(curr->lo & PAGE_FRAME), 
					PAGE_SIZE);

				vaddr_t perms = (curr->lo & CTRL_BIT_MASK) | TLBLO_VALID;
				page_table_insert(curr->hi & PAGE_FRAME, page_addr | perms, newas);
			}

			curr = curr->next;
		}
	}

	lock_release(page_table_lock);

	return 0;
}

/* deallocates page tables for a current adress space for a process */
void
as_destroy(struct addrspace *as)
{
	
	KASSERT(as != NULL);
  
	struct mem_region *curr, *prev;

	curr = as->head;

	/* freeing memory region */
	while (curr != NULL) {
		prev = curr;
		curr = curr->next;
		kfree(prev);
	}

	/**
	*	remove the page table entry that contains that specific memory address.
	*/
	lock_acquire(page_table_lock);

	for (uint32_t i = 0; i < hpt_size; i++) {
    if (page_table[i].lo == 0 || page_table[i].as == NULL) {
			continue;
		}

		/* clearing the first pte in collision list */
		int first_cleared = false;
		while (!first_cleared) {
			if (page_table[i].as == as) {

				/* only pte for the specific hash */
				if (page_table[i].next == NULL) {
					free_kpages(PADDR_TO_KVADDR(page_table[i].lo & PAGE_FRAME));
    	    		page_table[i].hi = TLBHI_INVALID(i % 64);
    	    		page_table[i].lo = TLBLO_INVALID();
    	    		page_table[i].as = NULL;
    	    		page_table[i].next = NULL;
					first_cleared = true;
					continue;
				}

				/* freeing pte in collision list */
				struct HPT *new_head = page_table[i].next;
				free_kpages(PADDR_TO_KVADDR(page_table[i].lo & PAGE_FRAME));
				page_table[i].hi = new_head->hi;
				page_table[i].lo = new_head->lo;
				page_table[i].as = new_head->as;
				page_table[i].next = new_head->next;
				kfree(new_head);

			} else {
				break;
			}
		}

		if (first_cleared) {
			/* skip this current interation */
			continue;
		}

		/* deleting pte within collision list */
		struct HPT *curr_pte = page_table[i].next, *prev = NULL;

		while (curr_pte != NULL) {
			if (curr_pte->as == as) {
				/* pte is the first node in list */
				if (prev == NULL) {
					page_table[i].next = curr_pte->next;
					free_kpages(PADDR_TO_KVADDR(curr_pte->lo & PAGE_FRAME));
					kfree(curr_pte);
					curr_pte = page_table[i].next;
				} else {
					prev->next = curr_pte->next;
					free_kpages(PADDR_TO_KVADDR(curr_pte->lo & PAGE_FRAME));
					kfree(curr_pte);
					curr_pte = prev->next;
				}
			}

			if (curr_pte == NULL) {
				continue;
			}
			prev = curr_pte;
			curr_pte = curr_pte->next;

		}

	}
	lock_release(page_table_lock);
	
	kfree(as);
}

/* function contents taken from dumbvm.c */
void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i = 0; i < NUM_TLB; i++) {
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
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{

	KASSERT(as != NULL);

	struct mem_region *curr_region = as->head;
	size_t npages;

  /* Align the region. First, the base... */
  memsize += vaddr & ~(vaddr_t)PAGE_FRAME;

  vaddr &= PAGE_FRAME;

  /* ...and now the length. */
  memsize = (memsize + PAGE_SIZE - 1) & PAGE_FRAME;
  npages = memsize / PAGE_SIZE;
	
	mode_t mode = readable | writeable | executable;

	struct mem_region *temp = init_region(vaddr, npages, mode, mode);

	if (temp == NULL) {
		return ENOMEM;
	}

	/* address space empty */
	if (curr_region == NULL) {
		as->head = temp;

		/* appending to end of region list */
	} else {
		while (curr_region->next != NULL) {
			curr_region = curr_region->next;
		}
		curr_region->next = temp;
	}

	return 0;
}

/* marks the address space as being read\write (overriding current permissions) initially */
int
as_prepare_load(struct addrspace *as)
{

	KASSERT(as != NULL);

	struct mem_region *curr_region;
	curr_region = as->head;

	while (curr_region != NULL) {
		curr_region->mode |= PF_W;
		curr_region = curr_region->next;
	}

	return 0;
}

/* marks the address space as being read only before running the code */
int
as_complete_load(struct addrspace *as)
{

	KASSERT(as != NULL);

	struct mem_region *curr_region;
	curr_region = as->head;

	while (curr_region != NULL) {
		curr_region->mode = curr_region->acc_mode;
		curr_region = curr_region->next;
	}

	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{

	KASSERT(as != NULL);

	struct mem_region *curr_region, *temp;
	curr_region = as->head;

	/* Finding last page block in list. */
	while (curr_region->next != NULL) {
		curr_region = curr_region->next;
	}

	vaddr_t stack_base, size;
	mode_t mode;

	/* defining new page block information. */ 
	stack_base = USERSPACETOP - (STACKPAGES * PAGE_SIZE);
	size = STACKPAGES;

	/* give stack writing permissions. */ 
	mode = PF_R | PF_W | PF_X;

	temp = init_region(stack_base, size, mode, mode);

	if (temp == NULL) {
		return ENOMEM;
	}

	/* adding new page to page block list. */ 
	curr_region->next = temp;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}

/* Initialising memory region */
struct mem_region *
init_region(vaddr_t vbase, vaddr_t size, mode_t mode, mode_t acc_mode)
{
	struct mem_region *page = kmalloc(sizeof(struct mem_region));

	if (page == NULL) {
		/* translate to return ENOMEM in composite function */
		return NULL;
	}

	page->vbase = vbase;
	page->size = size;
	page->mode = mode;
	page->acc_mode = acc_mode;
	page->next = NULL;

	return page;
}
