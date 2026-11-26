// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/anon_inodes.h>
#include <linux/kref.h>
#include <linux/sched/mm.h>
#include <linux/file.h>
#include <linux/kdev_t.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "gzvm_drv_v9_1.h"
#include "gzvm_common.h"

static DEFINE_MUTEX(gzvm_list_lock);
static LIST_HEAD(gzvm_list);

/* active_vms includes quarantined VMs: their firmware state is not gone. */
static atomic_t gzvm_active_vms = ATOMIC_INIT(0);
static atomic_t gzvm_quarantined_vms = ATOMIC_INIT(0);

static int gzvm_get_counter(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%d\n",
			 atomic_read((atomic_t *)kp->arg));
}

static const struct kernel_param_ops gzvm_counter_ops = {
	.get = gzvm_get_counter,
};
module_param_cb(active_vms, &gzvm_counter_ops, &gzvm_active_vms, 0444);
MODULE_PARM_DESC(active_vms, "VMs retained by files or a failed firmware teardown");
module_param_cb(quarantined_vms, &gzvm_counter_ops, &gzvm_quarantined_vms, 0444);
MODULE_PARM_DESC(quarantined_vms, "Failed firmware teardown: reboot required");

u64 gzvm_gfn_to_hva_memslot(struct gzvm_memslot *memslot, u64 gfn)
{
	u64 offset = gfn - memslot->base_gfn;

	return memslot->userspace_addr + offset * PAGE_SIZE;
}

/**
 * gzvm_find_memslot() - Find memslot containing this @gpa
 * @vm: Pointer to struct gzvm
 * @gfn: Guest frame number
 *
 * Return:
 * * >=0		- Index of memslot
 * * -EFAULT		- Not found
 */
int gzvm_find_memslot(struct gzvm *vm, u64 gfn)
{
	int i;

	for (i = 0; i < GZVM_MAX_MEM_REGION; i++) {
		if (vm->memslot[i].npages == 0)
			continue;

		if (gfn >= vm->memslot[i].base_gfn &&
		    gfn < vm->memslot[i].base_gfn + vm->memslot[i].npages)
			return i;
	}

	return -EFAULT;
}

/**
 * register_memslot_addr_range() - Register memory region to GenieZone
 * @gzvm: Pointer to struct gzvm
 * @memslot: Pointer to struct gzvm_memslot
 *
 * Return: 0 for success, negative number for error
 */
static int
register_memslot_addr_range(struct gzvm *gzvm, struct gzvm_memslot *memslot)
{
	struct gzvm_memory_region_ranges *region;
	u32 buf_size = PAGE_SIZE * 2;
	u64 gfn;

	region = alloc_pages_exact(buf_size, GFP_KERNEL | __GFP_ZERO);
	if (!region)
		return -ENOMEM;

	region->slot = memslot->slot_id;
	region->total_pages = memslot->npages;
	gfn = memslot->base_gfn;
	region->gpa = PFN_PHYS(gfn);

	if (gzvm_arch_set_memregion(gzvm->vm_id, buf_size,
				    virt_to_phys(region))) {
		pr_err("Failed to register memregion to hypervisor\n");
		free_pages_exact(region, buf_size);
		return -EFAULT;
	}

	free_pages_exact(region, buf_size);
	return 0;
}

/**
 * gzvm_vm_ioctl_set_memory_region() - Set memory region of guest
 * @gzvm: Pointer to struct gzvm.
 * @mem: Input memory region from user.
 *
 * Return: 0 for success, negative number for error
 *
 * -EXIO		- The memslot is out-of-range
 * -EFAULT		- Cannot find corresponding vma
 * -EINVAL		- Region size and VMA size mismatch
 */
static int
gzvm_vm_ioctl_set_memory_region(struct gzvm *gzvm,
			      struct gzvm_userspace_memory_region *mem)
{
	struct gzvm_memslot candidate = {0};
	struct gzvm_memslot *slot, *other;
	struct vm_area_struct *vma;
	u64 hva_end, gpa_end;
	int i, ret = 0;

	if (mem->slot >= GZVM_MAX_MEM_REGION)
		return -ENXIO;
	if (!mem->memory_size ||
	    !IS_ALIGNED(mem->guest_phys_addr, PAGE_SIZE) ||
	    !IS_ALIGNED(mem->userspace_addr, PAGE_SIZE) ||
	    !IS_ALIGNED(mem->memory_size, PAGE_SIZE))
		return -EINVAL;
	hva_end = mem->userspace_addr + mem->memory_size;
	gpa_end = mem->guest_phys_addr + mem->memory_size;
	if (hva_end <= mem->userspace_addr || gpa_end <= mem->guest_phys_addr ||
	    !access_ok((void __user *)mem->userspace_addr, mem->memory_size))
		return -EINVAL;

	candidate.base_gfn = PHYS_PFN(mem->guest_phys_addr);
	candidate.npages = mem->memory_size >> PAGE_SHIFT;
	candidate.userspace_addr = mem->userspace_addr;
	candidate.flags = mem->flags;
	candidate.slot_id = mem->slot;

	mutex_lock(&gzvm->mem_lock);
	if (READ_ONCE(gzvm->fatal)) {
		ret = -EIO;
		goto out;
	}
	slot = &gzvm->memslot[mem->slot];
	if (slot->npages) {
		/* No firmware unmap ABI has been established for replacing a slot. */
		if (slot->base_gfn != candidate.base_gfn ||
		    slot->npages != candidate.npages ||
		    slot->userspace_addr != candidate.userspace_addr ||
		    slot->flags != candidate.flags)
			ret = -EBUSY;
		goto out;
	}
	for (i = 0; i < GZVM_MAX_MEM_REGION; i++) {
		other = &gzvm->memslot[i];
		if (other->npages &&
		    candidate.base_gfn < other->base_gfn + other->npages &&
		    other->base_gfn < candidate.base_gfn + candidate.npages) {
			ret = -EEXIST;
			goto out;
		}
	}

	/* Retain no raw VMA pointer after dropping mmap_lock. */
	mmap_read_lock(gzvm->mm);
	vma = vma_lookup(gzvm->mm, mem->userspace_addr);
	if (!vma || hva_end > vma->vm_end)
		ret = -EFAULT;
	mmap_read_unlock(gzvm->mm);
	if (ret)
		goto out;

	ret = gzvm_arch_memregion_purpose(gzvm, mem);
	if (!ret)
		ret = register_memslot_addr_range(gzvm, &candidate);
	if (ret) {
		/* One HVC may have succeeded: do not reuse uncertain slot state. */
		WRITE_ONCE(gzvm->fatal, true);
		pr_err("gzvm_v9_1: VM %u slot %u registration failed (%d); close VM\n",
		       gzvm->vm_id, mem->slot, ret);
		goto out;
	}
	*slot = candidate;
out:
	mutex_unlock(&gzvm->mem_lock);
	return ret;
}

int gzvm_irqchip_inject_irq(struct gzvm *gzvm, unsigned int vcpu_idx,
			    u32 irq, bool level)
{
	return gzvm_arch_inject_irq(gzvm, vcpu_idx, irq, level);
}

static int gzvm_vm_ioctl_irq_line(struct gzvm *gzvm,
				  struct gzvm_irq_level *irq_level)
{
	u32 irq = irq_level->irq;
	u32 vcpu_idx, vcpu2_idx, irq_num;
	bool level = irq_level->level;

	vcpu_idx = FIELD_GET(GZVM_IRQ_LINE_VCPU, irq);
	vcpu2_idx = FIELD_GET(GZVM_IRQ_LINE_VCPU2, irq) * (GZVM_IRQ_VCPU_MASK + 1);
	irq_num = FIELD_GET(GZVM_IRQ_LINE_NUM, irq);

	return gzvm_irqchip_inject_irq(gzvm, vcpu_idx + vcpu2_idx, irq_num,
				       level);
}

static int gzvm_vm_ioctl_create_device(struct gzvm *gzvm, void __user *argp)
{
	struct gzvm_create_device *gzvm_dev;
	void *dev_data = NULL;
	size_t attr_size = 0;
	int ret;

	gzvm_dev = (struct gzvm_create_device *)alloc_pages_exact(PAGE_SIZE,
								  GFP_KERNEL | __GFP_ZERO);
	if (!gzvm_dev)
		return -ENOMEM;
	if (copy_from_user(gzvm_dev, argp, sizeof(*gzvm_dev))) {
		ret = -EFAULT;
		goto err_free_dev;
	}

	if (!!gzvm_dev->attr_addr != !!gzvm_dev->attr_size) {
		ret = -EINVAL;
		goto err_free_dev;
	}
	if (gzvm_dev->attr_addr != 0 && gzvm_dev->attr_size != 0) {
		void __user *attr_addr = (void __user *)gzvm_dev->attr_addr;

		attr_size = gzvm_dev->attr_size;

		/* Size of device specific data should not be over a page. */
		if (attr_size > PAGE_SIZE) {
			ret = -EINVAL;
			goto err_free_dev;
		}

		dev_data = alloc_pages_exact(attr_size, GFP_KERNEL | __GFP_ZERO);
		if (!dev_data) {
			ret = -ENOMEM;
			goto err_free_dev;
		}

		if (copy_from_user(dev_data, attr_addr, attr_size)) {
			ret = -EFAULT;
			goto err_free_dev_data;
		}
		gzvm_dev->attr_addr = virt_to_phys(dev_data);
	}

	ret = gzvm_arch_create_device(gzvm->vm_id, gzvm_dev);
err_free_dev_data:
	if (dev_data)
		free_pages_exact(dev_data, attr_size);
err_free_dev:
	free_pages_exact(gzvm_dev, PAGE_SIZE);
	return ret;
}

static int gzvm_vm_ioctl_enable_cap(struct gzvm *gzvm,
				    struct gzvm_enable_cap *cap,
				    void __user *argp)
{
	if (cap->cap == GZVM_CAP_PROTECTED_VM)
		return -EOPNOTSUPP;
	if (argp && cap->cap == GZVM_CAP_BLOCK_BASED_DEMAND_PAGING)
		return -EOPNOTSUPP;
	return gzvm_vm_ioctl_arch_enable_cap(gzvm, cap, argp);
}

/* gzvm_vm_ioctl() - Ioctl handler of VM FD */
static long gzvm_vm_ioctl(struct file *filp, unsigned int ioctl,
			  unsigned long arg)
{
	long ret;
	void __user *argp = (void __user *)arg;
	struct gzvm *gzvm = filp->private_data;

	if (gzvm->mm != current->mm)
		return -EIO;
	if (READ_ONCE(gzvm->fatal))
		return -EIO;

	switch (ioctl) {
	case GZVM_CHECK_EXTENSION: {
		ret = gzvm_dev_ioctl_check_extension(gzvm, arg);
		break;
	}
	case GZVM_CREATE_VCPU: {
		ret = gzvm_vm_ioctl_create_vcpu(gzvm, arg);
		break;
	}
	case GZVM_SET_USER_MEMORY_REGION: {
		struct gzvm_userspace_memory_region userspace_mem;

		if (copy_from_user(&userspace_mem, argp, sizeof(userspace_mem))) {
			ret = -EFAULT;
			goto out;
		}
		ret = gzvm_vm_ioctl_set_memory_region(gzvm, &userspace_mem);
		break;
	}
	case GZVM_IRQ_LINE: {
		struct gzvm_irq_level irq_event;

		if (copy_from_user(&irq_event, argp, sizeof(irq_event))) {
			ret = -EFAULT;
			goto out;
		}
		ret = gzvm_vm_ioctl_irq_line(gzvm, &irq_event);
		break;
	}
	case GZVM_CREATE_DEVICE: {
		ret = gzvm_vm_ioctl_create_device(gzvm, argp);
		break;
	}
	case GZVM_IRQFD: {
		struct gzvm_irqfd data;

		if (copy_from_user(&data, argp, sizeof(data))) {
			ret = -EFAULT;
			goto out;
		}
		ret = gzvm_irqfd(gzvm, &data);
		break;
	}
	case GZVM_IOEVENTFD: {
		struct gzvm_ioeventfd data;

		if (copy_from_user(&data, argp, sizeof(data))) {
			ret = -EFAULT;
			goto out;
		}
		ret = gzvm_ioeventfd(gzvm, &data);
		break;
	}
	case GZVM_ENABLE_CAP: {
		struct gzvm_enable_cap cap;

		if (copy_from_user(&cap, argp, sizeof(cap))) {
			ret = -EFAULT;
			goto out;
		}
		ret = gzvm_vm_ioctl_enable_cap(gzvm, &cap, argp);
		break;
	}
	case GZVM_SET_DTB_CONFIG: {
		struct gzvm_dtb_config cfg;

		if (copy_from_user(&cfg, argp, sizeof(cfg))) {
			ret = -EFAULT;
			goto out;
		}
		ret = gzvm_arch_set_dtb_config(gzvm, &cfg);
		break;
	}
	default:
		ret = -ENOTTY;
	}
out:
	return ret;
}

static void gzvm_destroy_ppage(struct gzvm *gzvm)
{
	struct gzvm_pinned_page *ppage;
	struct rb_node *node;

	node = rb_first(&gzvm->pinned_pages);
	while (node) {
		ppage = rb_entry(node, struct gzvm_pinned_page, node);
		unpin_user_pages_dirty_lock(&ppage->page, 1, true);
		node = rb_next(node);
		rb_erase(&ppage->node, &gzvm->pinned_pages);
		kfree(ppage);
	}
}

static void gzvm_destroy_vm(struct kref *ref)
{
	struct gzvm *gzvm = container_of(ref, struct gzvm, refcount);
	size_t allocated_size;
	int i, ret;

	/* Last VM/vCPU file reference: no userspace ioctl can still use this VM. */
	gzvm_vm_irqfd_release(gzvm);
	gzvm_ioeventfd_release(gzvm);

	/* Keep every shared buffer until the final VM destroy is acknowledged. */
	for (i = 0; i < GZVM_MAX_VCPUS; i++) {
		if (!gzvm->vcpus[i])
			continue;
		hrtimer_cancel(&gzvm->vcpus[i]->legacy_vtimer);
		ret = gzvm_arch_destroy_vcpu(gzvm->vm_id, i);
		if (ret)
			pr_warn("gzvm_v9_1: VM %u vCPU %d destroy returned %d; awaiting VM destroy\n",
				gzvm->vm_id, i, ret);
	}
	ret = gzvm_arch_destroy_vm(gzvm->vm_id);
	if (ret) {
		WRITE_ONCE(gzvm->fatal, true);
		atomic_inc(&gzvm_quarantined_vms);
		/* A file's module reference still protects us during this callback. */
		__module_get(THIS_MODULE);
		pr_err("gzvm_v9_1: VM %u destroy failed (%d); retaining shared pages and module; reboot required\n",
		       gzvm->vm_id, ret);
		return;
	}

	cleanup_srcu_struct(&gzvm->irq_srcu);
	gzvm_destroy_vcpus(gzvm);
	if (gzvm->demand_page_buffer) {
		allocated_size = GZVM_BLOCK_BASED_DEMAND_PAGE_SIZE / PAGE_SIZE * sizeof(u64);
		free_pages_exact(gzvm->demand_page_buffer, allocated_size);
	}
	mutex_lock(&gzvm->mem_lock);
	gzvm_destroy_ppage(gzvm);
	mutex_unlock(&gzvm->mem_lock);

	mutex_lock(&gzvm_list_lock);
	list_del(&gzvm->vm_list);
	mutex_unlock(&gzvm_list_lock);
	mmdrop(gzvm->mm);
	atomic_dec(&gzvm_active_vms);
	pr_info("gzvm_v9_1: VM %u destroyed; shared buffers and pins released\n",
		gzvm->vm_id);
	kfree(gzvm);
}

void gzvm_get_vm(struct gzvm *gzvm)
{
	kref_get(&gzvm->refcount);
}

void gzvm_put_vm(struct gzvm *gzvm)
{
	kref_put(&gzvm->refcount, gzvm_destroy_vm);
}

static int gzvm_vm_release(struct inode *inode, struct file *filp)
{
	struct gzvm *gzvm = filp->private_data;

	if (gzvm)
		gzvm_put_vm(gzvm);
	return 0;
}

static const struct file_operations gzvm_vm_fops = {
	.owner          = THIS_MODULE,
	.release        = gzvm_vm_release,
	.unlocked_ioctl = gzvm_vm_ioctl,
	.llseek		= noop_llseek,
};

/**
 * setup_vm_demand_paging - Query hypervisor suitable demand page size and set
 * @vm: gzvm instance for setting up demand page size
 *
 * Return: void
 */
static void setup_vm_demand_paging(struct gzvm *vm)
{
	u32 buf_size = GZVM_BLOCK_BASED_DEMAND_PAGE_SIZE / PAGE_SIZE * sizeof(u64);
	struct gzvm_enable_cap cap = {0};
	void *buffer;
	int ret;

	mutex_init(&vm->demand_paging_lock);
	buffer = alloc_pages_exact(buf_size, GFP_KERNEL | __GFP_ZERO);
	if (!buffer) {
		/* Fall back to use default page size for demand paging */
		vm->demand_page_gran = PAGE_SIZE;
		vm->demand_page_buffer = NULL;
		return;
	}

	cap.cap = GZVM_CAP_BLOCK_BASED_DEMAND_PAGING;
	cap.args[0] = GZVM_BLOCK_BASED_DEMAND_PAGE_SIZE;
	cap.args[1] = (__u64)virt_to_phys(buffer);
	/* demand_page_buffer is freed when destroy VM */
	vm->demand_page_buffer = buffer;

	ret = gzvm_vm_ioctl_enable_cap(vm, &cap, NULL);
	if (ret == 0) {
		vm->demand_page_gran = GZVM_BLOCK_BASED_DEMAND_PAGE_SIZE;
		/* freed when destroy vm */
		vm->demand_page_buffer = buffer;
	} else {
		vm->demand_page_gran = PAGE_SIZE;
		/* The mailbox address was submitted to firmware. Retain it until
		 * VM destruction is acknowledged even when capability setup fails.
		 */
	}
}

static struct gzvm *gzvm_create_vm(unsigned long vm_type, u32 quirks)
{
	struct gzvm *gzvm;
	int ret;

	if (!current->mm || atomic_read(&gzvm_quarantined_vms))
		return ERR_PTR(-EIO);
	gzvm = kzalloc(sizeof(*gzvm), GFP_KERNEL);
	if (!gzvm)
		return ERR_PTR(-ENOMEM);

	gzvm->quirks = quirks;
	gzvm->mm = current->mm;
	mmgrab(gzvm->mm);
	kref_init(&gzvm->refcount);
	mutex_init(&gzvm->lock);
	mutex_init(&gzvm->mem_lock);
	gzvm->pinned_pages = RB_ROOT;
	INIT_LIST_HEAD(&gzvm->vm_list);

	/* Complete fallible host initialization before creating firmware state. */
	ret = gzvm_vm_irqfd_init(gzvm);
	if (ret)
		goto err_mm;
	ret = gzvm_init_ioeventfd(gzvm);
	if (ret)
		goto err_srcu;

	ret = gzvm_arch_create_vm(vm_type);
	if (ret < 0)
		goto err_srcu;
	gzvm->vm_id = ret;
	atomic_inc(&gzvm_active_vms);

	setup_vm_demand_paging(gzvm);
	mutex_lock(&gzvm_list_lock);
	list_add(&gzvm->vm_list, &gzvm_list);
	mutex_unlock(&gzvm_list_lock);

	pr_debug("VM-%u is created\n", gzvm->vm_id);
	return gzvm;

err_srcu:
	cleanup_srcu_struct(&gzvm->irq_srcu);
err_mm:
	mmdrop(gzvm->mm);
	kfree(gzvm);
	return ERR_PTR(ret);
}

/**
 * gzvm_dev_ioctl_create_vm - Create vm fd
 * @vm_type: VM type. Only supports Linux VM now.
 * @quirks: Firmware ABI flags selected by the device file.
 *
 * Return: fd of vm, negative if error
 */
int gzvm_dev_ioctl_create_vm(unsigned long vm_type, u32 quirks)
{
	struct gzvm *gzvm;
	struct file *file;
	int fd, ret;

	/* Reserve every fallible file resource before the HVC creates a VM. */
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;
	file = anon_inode_getfile("gzvm-vm", &gzvm_vm_fops, NULL, O_RDWR);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto err_fd;
	}
	gzvm = gzvm_create_vm(vm_type, quirks);
	if (IS_ERR(gzvm)) {
		ret = PTR_ERR(gzvm);
		fput(file);
		goto err_fd;
	}

	file->private_data = gzvm;
	fd_install(fd, file);
	return fd;

err_fd:
	put_unused_fd(fd);
	return ret;
}
