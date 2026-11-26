// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 * Local v9.1 adaptation for the 6.1 GenieZone host driver.
 */

#include <linux/atomic.h>
#include <linux/ratelimit.h>

#include "gzvm_drv_v9_1.h"

/*
 * These short-lived translations are used only by the original protected-VM
 * population path.  They do not replace long-term pins for a runnable guest.
 * The v9.1 target disables protected-VM setup in arch_enable_cap().
 */
u64 hva_to_pa_fast(u64 hva)
{
	struct page *page[1];
	u64 pa;

	if (get_user_page_fast_only(hva, 0, page)) {
		pa = page_to_phys(page[0]);
		put_page(page[0]);
		return pa;
	}
	return GZVM_PA_ERR_BAD;
}

u64 hva_to_pa_slow(u64 hva)
{
	struct page *page = NULL;
	u64 pa;
	int npages;

	npages = get_user_pages_unlocked(hva, 1, &page, 0);
	if (npages != 1 || !page)
		return GZVM_PA_ERR_BAD;

	pa = page_to_phys(page);
	put_page(page);
	return pa;
}

static u64 __gzvm_gfn_to_pfn_memslot(struct gzvm_memslot *memslot, u64 gfn)
{
	u64 hva, pa;

	hva = gzvm_gfn_to_hva_memslot(memslot, gfn);
	pa = gzvm_hva_to_pa_arch(hva);
	if (pa != GZVM_PA_ERR_BAD)
		return PHYS_PFN(pa);
	pa = hva_to_pa_fast(hva);
	if (pa != GZVM_PA_ERR_BAD)
		return PHYS_PFN(pa);
	pa = hva_to_pa_slow(hva);
	if (pa != GZVM_PA_ERR_BAD)
		return PHYS_PFN(pa);
	return GZVM_PA_ERR_BAD;
}

int gzvm_gfn_to_pfn_memslot(struct gzvm_memslot *memslot, u64 gfn, u64 *pfn)
{
	u64 host_pfn;

	if (!memslot || gfn < memslot->base_gfn ||
	    gfn - memslot->base_gfn >= memslot->npages)
		return -EFAULT;

	host_pfn = __gzvm_gfn_to_pfn_memslot(memslot, gfn);
	if (host_pfn == GZVM_PA_ERR_BAD) {
		*pfn = 0;
		return -EFAULT;
	}
	*pfn = host_pfn;
	return 0;
}

static int cmp_ppages(struct rb_node *node, const struct rb_node *parent)
{
	struct gzvm_pinned_page *a = rb_entry(node, struct gzvm_pinned_page, node);
	struct gzvm_pinned_page *b = rb_entry(parent, struct gzvm_pinned_page, node);

	return (a->ipa > b->ipa) - (a->ipa < b->ipa);
}

static int rb_ppage_cmp(const void *key, const struct rb_node *node)
{
	const struct gzvm_pinned_page *p =
		rb_entry(node, struct gzvm_pinned_page, node);
	phys_addr_t ipa = (phys_addr_t)(unsigned long)key;

	return (ipa > p->ipa) - (ipa < p->ipa);
}

/*
 * Caller holds mem_lock from slot lookup through the HVC that submits pages.
 * *new_pin is non-NULL only for a newly pinned page not yet submitted by this
 * request.  A caller may undo that pin before the HVC.  An HVC error does not
 * prove that firmware has dropped its mapping, so submitted pins stay in the
 * tree until a relinquish event or successful VM destruction.
 */
static int pin_one_page(struct gzvm *vm, unsigned long hva, u64 gpa,
			struct page **out_page,
			struct gzvm_pinned_page **new_pin)
{
	unsigned int flags = FOLL_HWPOISON | FOLL_LONGTERM | FOLL_WRITE;
	struct gzvm_pinned_page *ppage = NULL, *existing = NULL;
	struct rb_node *node;
	struct page *page = NULL;
	long nr;

	lockdep_assert_held(&vm->mem_lock);
	*new_pin = NULL;
	*out_page = NULL;
	if (current->mm != vm->mm)
		return -EIO;

	node = rb_find((void *)(unsigned long)gpa, &vm->pinned_pages,
		      rb_ppage_cmp);
	if (node) {
		existing = rb_entry(node, struct gzvm_pinned_page, node);
	} else {
		ppage = kmalloc(sizeof(*ppage), GFP_KERNEL_ACCOUNT);
		if (!ppage)
			return -ENOMEM;
	}

	/* FOLL_WRITE resolves any COW before the chosen PFN reaches firmware. */
	mmap_read_lock(vm->mm);
	nr = pin_user_pages(hva, 1, flags, &page, NULL);
	mmap_read_unlock(vm->mm);
	if (nr != 1 || !page) {
		if (nr > 0 && page)
			unpin_user_pages(&page, 1);
		kfree(ppage);
		return nr < 0 ? (int)nr : -EFAULT;
	}

	if (existing) {
		bool same_page = existing->page == page;

		/* Keep exactly one persistent pin per GPA. */
		unpin_user_pages(&page, 1);
		if (!same_page) {
			pr_warn_ratelimited("gzvm_v9_1: backing changed vm=%u gpa=0x%llx\n",
					    vm->vm_id, (unsigned long long)gpa);
			return -EFAULT;
		}
		*out_page = existing->page;
		return 0;
	}

	ppage->page = page;
	ppage->ipa = gpa;
	if (WARN_ON_ONCE(rb_find_add(&ppage->node, &vm->pinned_pages,
				    cmp_ppages))) {
		unpin_user_pages(&page, 1);
		kfree(ppage);
		return -EEXIST;
	}
	*out_page = page;
	*new_pin = ppage;
	return 0;
}

/* Only for a pin that this request has never submitted to the hypervisor. */
static void undo_unsubmitted_pin(struct gzvm *vm, struct gzvm_pinned_page *p)
{
	lockdep_assert_held(&vm->mem_lock);
	if (!p)
		return;
	rb_erase(&p->node, &vm->pinned_pages);
	unpin_user_pages(&p->page, 1);
	kfree(p);
}

int gzvm_handle_relinquish(struct gzvm_vcpu *vcpu, phys_addr_t ipa)
{
	struct gzvm *vm = vcpu->gzvm;
	struct gzvm_pinned_page *p;
	struct rb_node *node;

	if (!IS_ALIGNED(ipa, PAGE_SIZE))
		return -EINVAL;
	mutex_lock(&vm->mem_lock);
	node = rb_find((void *)(unsigned long)ipa, &vm->pinned_pages,
		      rb_ppage_cmp);
	if (node) {
		p = rb_entry(node, struct gzvm_pinned_page, node);
		rb_erase(node, &vm->pinned_pages);
		/* The existing ABI's relinquish event returns ownership of a page. */
		unpin_user_pages_dirty_lock(&p->page, 1, true);
		kfree(p);
	}
	mutex_unlock(&vm->mem_lock);
	return 0;
}

static atomic64_t diag_single = ATOMIC64_INIT(0);
static atomic64_t diag_block = ATOMIC64_INIT(0);

static void diag_map(struct gzvm *vm, const char *kind, u64 gfn,
		     u32 nr_pages, int ret, atomic64_t *counter)
{
	u64 n = atomic64_inc_return(counter);

	if (ret) {
		pr_warn_ratelimited("gzvm_v9_1: map=%s vm=%u gpa=0x%llx pages=%u ret=%d; pins retained\n",
				    kind, vm->vm_id,
				    (unsigned long long)PFN_PHYS(gfn),
				    nr_pages, ret);
	} else if (n <= 8 || !(n & (n - 1))) {
		pr_info("gzvm_v9_1: map=%s count=%llu vm=%u gpa=0x%llx pages=%u ret=0\n",
			kind, (unsigned long long)n, vm->vm_id,
			(unsigned long long)PFN_PHYS(gfn), nr_pages);
	}
}

static int handle_single_demand_page(struct gzvm *vm, int slot_id, u64 gfn)
{
	struct gzvm_pinned_page *new_pin;
	struct page *page;
	unsigned long hva;
	int ret;

	lockdep_assert_held(&vm->mem_lock);
	hva = gzvm_gfn_to_hva_memslot(&vm->memslot[slot_id], gfn);
	ret = pin_one_page(vm, hva, PFN_PHYS(gfn), &page, &new_pin);
	if (ret)
		return ret;

	/* Keep new_pin even on failure: firmware may have accepted this PFN. */
	ret = gzvm_arch_map_guest(vm->vm_id, slot_id, page_to_pfn(page), gfn, 1);
	if (ret)
		WRITE_ONCE(vm->fatal, true);
	diag_map(vm, "4K", gfn, 1, ret, &diag_single);
	return ret;
}

static int handle_block_demand_page(struct gzvm *vm, int slot_id, u64 gfn)
{
	struct gzvm_memslot *slot = &vm->memslot[slot_id];
	const u32 max_entries = GZVM_BLOCK_BASED_DEMAND_PAGE_SIZE / PAGE_SIZE;
	u64 block_start = ALIGN_DOWN(gfn, max_entries);
	u64 block_end = block_start + max_entries;
	u64 slot_end = slot->base_gfn + slot->npages;
	u64 start = max(block_start, slot->base_gfn);
	u64 end = min(block_end, slot_end);
	struct gzvm_pinned_page **new_pins;
	struct page *page;
	u32 count, i;
	unsigned long hva;
	int ret;

	lockdep_assert_held(&vm->mem_lock);
	if (!vm->demand_page_buffer || end <= start || gfn < start || gfn >= end)
		return -EFAULT;
	count = end - start;
	new_pins = kcalloc(count, sizeof(*new_pins), GFP_KERNEL_ACCOUNT);
	if (!new_pins)
		return -ENOMEM;

	/* mem_lock also serializes access to the shared demand-page mailbox. */
	for (i = 0; i < count; i++) {
		hva = gzvm_gfn_to_hva_memslot(slot, start + i);
		ret = pin_one_page(vm, hva, PFN_PHYS(start + i), &page, &new_pins[i]);
		if (ret)
			goto undo_before_hvc;
		vm->demand_page_buffer[i] = page_to_pfn(page);
	}

	ret = gzvm_arch_map_guest_block(vm->vm_id, slot_id, start, count);
	if (ret)
		WRITE_ONCE(vm->fatal, true);
	diag_map(vm, "2M", start, count, ret, &diag_block);
	/* Never unpin submitted pages solely because this HVC returned an error. */
	kfree(new_pins);
	return ret;

undo_before_hvc:
	while (i > 0)
		undo_unsubmitted_pin(vm, new_pins[--i]);
	kfree(new_pins);
	return ret;
}

int gzvm_handle_page_fault(struct gzvm_vcpu *vcpu)
{
	struct gzvm *vm = vcpu->gzvm;
	u64 gfn = PHYS_PFN(vcpu->run->exception.fault_gpa);
	int slot_id, ret;

	if (current->mm != vm->mm)
		return -EIO;
	mutex_lock(&vm->mem_lock);
	if (READ_ONCE(vm->fatal)) {
		ret = -EIO;
		goto out;
	}
	slot_id = gzvm_find_memslot(vm, gfn);
	if (slot_id < 0) {
		ret = -EFAULT;
		goto out;
	}
	if (vm->demand_page_gran == PAGE_SIZE)
		ret = handle_single_demand_page(vm, slot_id, gfn);
	else if (vm->demand_page_gran == GZVM_BLOCK_BASED_DEMAND_PAGE_SIZE)
		ret = handle_block_demand_page(vm, slot_id, gfn);
	else
		ret = -EINVAL;
out:
	mutex_unlock(&vm->mem_lock);
	return ret;
}
