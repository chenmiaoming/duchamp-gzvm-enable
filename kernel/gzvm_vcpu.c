// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <asm/sysreg.h>
#include <linux/anon_inodes.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include "gzvm_drv_v9_1.h"

/* maximum size needed for holding an integer */
#define ITOA_MAX_LEN 12

static long gzvm_vcpu_update_one_reg(struct gzvm_vcpu *vcpu,
				     void __user *argp,
				     bool is_write)
{
	struct gzvm_one_reg reg;
	void __user *reg_addr;
	u64 data = 0;
	u64 reg_size;
	long ret;

	if (copy_from_user(&reg, argp, sizeof(reg)))
		return -EFAULT;

	reg_addr = (void __user *)reg.addr;
	reg_size = (reg.id & GZVM_REG_SIZE_MASK) >> GZVM_REG_SIZE_SHIFT;
	reg_size = BIT(reg_size);

	if (reg_size != 1 && reg_size != 2 && reg_size != 4 && reg_size != 8)
		return -EINVAL;

	if (is_write) {
		/* GZ hypervisor would filter out invalid vcpu register access */
		if (copy_from_user(&data, reg_addr, reg_size))
			return -EFAULT;
	} else {
		return -EOPNOTSUPP;
	}

	ret = gzvm_arch_vcpu_update_one_reg(vcpu, reg.id, is_write, &data);

	if (ret)
		return ret;

	return 0;
}

/**
 * gzvm_vcpu_handle_mmio() - Handle mmio in kernel space.
 * @vcpu: Pointer to vcpu.
 *
 * Return:
 * * true - This mmio exit has been processed.
 * * false - This mmio exit has not been processed, require userspace.
 */
static bool gzvm_vcpu_handle_mmio(struct gzvm_vcpu *vcpu)
{
	__u64 addr;
	__u32 len;
	const void *val_ptr;

	/* So far, we don't have in-kernel mmio read handler */
	if (!vcpu->run->mmio.is_write)
		return false;
	addr = vcpu->run->mmio.phys_addr;
	len = vcpu->run->mmio.size;
	val_ptr = &vcpu->run->mmio.data;

	return gzvm_ioevent_write(vcpu, addr, len, val_ptr);
}

/**
 * gzvm_vcpu_run() - Handle vcpu run ioctl, entry point to guest and exit
 *		     point from guest
 * @vcpu: Pointer to struct gzvm_vcpu
 * @argp: Pointer to struct gzvm_vcpu_run in userspace
 *
 * Return:
 * * 0			- Success.
 * * Negative		- Failure.
 */
static long gzvm_vcpu_run(struct gzvm_vcpu *vcpu, void __user *argp)
{
	struct gzvm_vcpu_run run;
	bool need_userspace = false;
	u64 exit_reason = 0;
	int ret;

	if (copy_from_user(&run, argp, sizeof(run)))
		return -EFAULT;

	for (int i = 0; i < ARRAY_SIZE(run.padding1); i++) {
		if (run.padding1[i])
			return -EINVAL;
	}

	if (run.immediate_exit == 1)
		return -EINTR;

	/*
	 * Userspace only fills the requested bytes of an MMIO read. Some
	 * legacy firmware writes the entire response into the guest register.
	 */
	if (run.exit_reason == GZVM_EXIT_MMIO && !run.mmio.is_write &&
	    run.mmio.size > 0 && run.mmio.size < sizeof(run.mmio.data))
		memset(run.mmio.data + run.mmio.size, 0,
		       sizeof(run.mmio.data) - run.mmio.size);

	memcpy(vcpu->run, &run, sizeof(run));
	while (!need_userspace && !signal_pending(current)) {
		if (READ_ONCE(vcpu->gzvm->fatal))
			return -EIO;
		ret = gzvm_arch_vcpu_run(vcpu, &exit_reason);
		if (ret) {
			pr_err_ratelimited("gzvm_v9_1: RUN failed vm=%u vcpu=%d ret=%d\n",
					   vcpu->gzvm->vm_id, vcpu->vcpuid, ret);
			return ret;
		}
		if (exit_reason == GZVM_EXIT_MMIO ||
		    exit_reason == GZVM_EXIT_HYPERCALL)
			vcpu->diag_same_gpa = 0;

		switch (exit_reason) {
		case GZVM_EXIT_MMIO:
			if (!gzvm_vcpu_handle_mmio(vcpu))
				need_userspace = true;
			break;
		/**
		 * it's geniezone's responsibility to fill corresponding data
		 * structure
		 */
		case GZVM_EXIT_HYPERCALL:
			if (!gzvm_handle_guest_hvc(vcpu))
				need_userspace = true;
			break;
		case GZVM_EXIT_EXCEPTION:
			if (!gzvm_handle_guest_exception(vcpu))
				need_userspace = true;
			break;
		case GZVM_EXIT_DEBUG:
			fallthrough;
		case GZVM_EXIT_FAIL_ENTRY:
			fallthrough;
		case GZVM_EXIT_INTERNAL_ERROR:
			fallthrough;
		case GZVM_EXIT_SYSTEM_EVENT:
			fallthrough;
		case GZVM_EXIT_SHUTDOWN:
			need_userspace = true;
			break;
		case GZVM_EXIT_IRQ:
			fallthrough;
		case GZVM_EXIT_GZ:
			break;
		case GZVM_EXIT_UNKNOWN:
			fallthrough;
		default:
			pr_err_ratelimited("gzvm_v9_1: unknown exit vm=%u vcpu=%d reason=0x%llx\n",
					   vcpu->gzvm->vm_id, vcpu->vcpuid,
					   (unsigned long long)exit_reason);
			need_userspace = true;
			goto out;
		}
	}

out:
	if (copy_to_user(argp, vcpu->run, sizeof(struct gzvm_vcpu_run)))
		return -EFAULT;
	if (signal_pending(current)) {
		// invoke hvc to inform gz to map memory
		gzvm_arch_inform_exit(vcpu->gzvm->vm_id);
		return -ERESTARTSYS;
	}
	return 0;
}

static long gzvm_vcpu_ioctl(struct file *filp, unsigned int ioctl,
			    unsigned long arg)
{
	int ret = -ENOTTY;
	void __user *argp = (void __user *)arg;
	struct gzvm_vcpu *vcpu = filp->private_data;

	if (vcpu->gzvm->mm != current->mm)
		return -EIO;
	ret = mutex_lock_killable(&vcpu->lock);
	if (ret)
		return ret;
	if (READ_ONCE(vcpu->gzvm->fatal)) {
		ret = -EIO;
		goto out_unlock;
	}
	ret = -ENOTTY;
	switch (ioctl) {
	case GZVM_RUN:
		ret = gzvm_vcpu_run(vcpu, argp);
		break;
	case GZVM_GET_ONE_REG:
		/* !is_write */
		ret = -EOPNOTSUPP;
		break;
	case GZVM_SET_ONE_REG:
		/* is_write */
		ret = gzvm_vcpu_update_one_reg(vcpu, argp, true);
		break;
	default:
		break;
	}

out_unlock:
	mutex_unlock(&vcpu->lock);
	return ret;
}

static int gzvm_vcpu_release(struct inode *inode, struct file *filp)
{
	struct gzvm_vcpu *vcpu = filp->private_data;

	/* NULL belongs to an uninstalled file whose HVC creation failed. */
	if (vcpu)
		gzvm_put_vm(vcpu->gzvm);
	return 0;
}

static const struct file_operations gzvm_vcpu_fops = {
	.owner = THIS_MODULE,
	.release = gzvm_vcpu_release,
	.unlocked_ioctl = gzvm_vcpu_ioctl,
	.llseek		= noop_llseek,
};

/* Called only after the final VM HVC destroy has succeeded. */
static void gzvm_destroy_vcpu(struct gzvm_vcpu *vcpu)
{
	if (!vcpu)
		return;

	/* clean guest's data */
	memset(vcpu->run, 0, GZVM_VCPU_RUN_MAP_SIZE);
	free_pages_exact(vcpu->run, GZVM_VCPU_RUN_MAP_SIZE);
	kfree(vcpu);
}

/**
 * gzvm_destroy_vcpus() - Free all vcpus after acknowledged firmware VM destruction
 *
 * @gzvm: vm struct that owns the vcpus
 */
void gzvm_destroy_vcpus(struct gzvm *gzvm)
{
	int i;

	for (i = 0; i < GZVM_MAX_VCPUS; i++) {
		gzvm_destroy_vcpu(gzvm->vcpus[i]);
		gzvm->vcpus[i] = NULL;
	}
}

/**
 * gzvm_vm_ioctl_create_vcpu() - reserve a file, create firmware state, publish
 * @gzvm: Parent VM; its ioctl file reference remains held by the caller.
 * @cpuid: vCPU slot to create.
 *
 * vCPU objects live until the final VM/vCPU file release. Closing a vCPU fd
 * drops its VM reference, but does not permit reusing its occupied slot.
 */
int gzvm_vm_ioctl_create_vcpu(struct gzvm *gzvm, u32 cpuid)
{
	char name[10 + ITOA_MAX_LEN + 1];
	struct gzvm_vcpu *vcpu;
	struct file *file;
	int fd, ret;

	if (cpuid >= GZVM_MAX_VCPUS)
		return -EINVAL;
	ret = mutex_lock_killable(&gzvm->lock);
	if (ret)
		return ret;
	if (READ_ONCE(gzvm->fatal)) {
		ret = -EIO;
		goto unlock;
	}
	if (gzvm->vcpus[cpuid]) {
		ret = -EEXIST;
		goto unlock;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto unlock;
	}
	vcpu = kzalloc(sizeof(*vcpu), GFP_KERNEL);
	if (!vcpu) {
		ret = -ENOMEM;
		goto free_fd;
	}
	vcpu->run = alloc_pages_exact(GZVM_VCPU_RUN_MAP_SIZE,
				      GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!vcpu->run) {
		ret = -ENOMEM;
		goto free_vcpu;
	}
	vcpu->hwstate = (void *)vcpu->run + PAGE_SIZE;
	vcpu->vcpuid = cpuid;
	vcpu->gzvm = gzvm;
	mutex_init(&vcpu->lock);
	gzvm_arch_init_vcpu_timer(vcpu);

	/* No allocation or fd operation may fail after CREATE_VCPU succeeds. */
	snprintf(name, sizeof(name), "gzvm-vcpu:%d", vcpu->vcpuid);
	file = anon_inode_getfile(name, &gzvm_vcpu_fops, NULL, O_RDWR);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto free_vcpu_run;
	}
	ret = gzvm_arch_create_vcpu(gzvm->vm_id, vcpu->vcpuid, vcpu->run);
	if (ret < 0) {
		/* The shared run pages were submitted to firmware. Keep them
		 * until a full VM destroy is acknowledged, even on CREATE error.
		 */
		gzvm->vcpus[cpuid] = vcpu;
		WRITE_ONCE(gzvm->fatal, true);
		pr_err("gzvm_v9_1: VM %u vCPU %u create failed (%d); close VM\n",
		       gzvm->vm_id, cpuid, ret);
		fput(file);
		put_unused_fd(fd);
		goto unlock;
	}

	gzvm->vcpus[cpuid] = vcpu;
	gzvm_get_vm(gzvm);
	file->private_data = vcpu;
	fd_install(fd, file);
	ret = fd;
	goto unlock;

free_vcpu_run:
	free_pages_exact(vcpu->run, GZVM_VCPU_RUN_MAP_SIZE);
free_vcpu:
	kfree(vcpu);
free_fd:
	put_unused_fd(fd);
unlock:
	mutex_unlock(&gzvm->lock);
	return ret;
}
