// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/arm-smccc.h>
#include <linux/err.h>
#include <linux/uaccess.h>

#include <linux/gzvm.h>
#include "gzvm_drv_v9_1.h"
#include "gzvm_arch_common.h"

#define PAR_PA47_MASK ((((1UL << 48) - 1) >> 12) << 12)

int gzvm_arch_inform_exit(u16 vm_id)
{
	struct arm_smccc_res res;

	arm_smccc_hvc(MT_HVC_GZVM_INFORM_EXIT, vm_id, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0 == 0)
		return 0;

	return -ENXIO;
}

int gzvm_arch_probe(void)
{
	struct arm_smccc_res res;

	arm_smccc_hvc(MT_HVC_GZVM_PROBE, 0, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		return -ENXIO;

	return 0;
}

int gzvm_arch_set_memregion(u16 vm_id, size_t buf_size,
			    phys_addr_t region)
{
	struct arm_smccc_res res;

	return gzvm_hypcall_wrapper(MT_HVC_GZVM_SET_MEMREGION, vm_id,
				    buf_size, region, 0, 0, 0, 0, &res);
}

static int gzvm_cap_vm_gpa_size(void __user *argp)
{
	__u64 value = CONFIG_ARM64_PA_BITS;

	if (copy_to_user(argp, &value, sizeof(__u64)))
		return -EFAULT;

	return 0;
}

int gzvm_arch_check_extension(struct gzvm *gzvm, __u64 cap, void __user *argp)
{
	int ret;

	switch (cap) {
	case GZVM_CAP_PROTECTED_VM:
		return -EOPNOTSUPP;
	case GZVM_CAP_VM_GPA_SIZE: {
		ret = gzvm_cap_vm_gpa_size(argp);
		return ret;
	}
	default:
		break;
	}

	return -EOPNOTSUPP;
}

/**
 * gzvm_arch_create_vm() - create vm
 * @vm_type: VM type. Only supports Linux VM now.
 *
 * Return:
 * * positive value	- VM ID
 * * -ENOMEM		- Memory not enough for storing VM data
 */
int gzvm_arch_create_vm(unsigned long vm_type)
{
	struct arm_smccc_res res;
	int ret;

	ret = gzvm_hypcall_wrapper(MT_HVC_GZVM_CREATE_VM, vm_type, 0, 0, 0, 0,
				   0, 0, &res);
	return ret ? ret : res.a1;
}

int gzvm_arch_destroy_vm(u16 vm_id)
{
	struct arm_smccc_res res;

	return gzvm_hypcall_wrapper(MT_HVC_GZVM_DESTROY_VM, vm_id, 0, 0, 0, 0,
				    0, 0, &res);
}

int gzvm_arch_memregion_purpose(struct gzvm *gzvm,
				struct gzvm_userspace_memory_region *mem)
{
	struct arm_smccc_res res;

	return gzvm_hypcall_wrapper(MT_HVC_GZVM_MEMREGION_PURPOSE, gzvm->vm_id,
				    mem->guest_phys_addr, mem->memory_size,
				    mem->flags, 0, 0, 0, &res);
}

int gzvm_arch_set_dtb_config(struct gzvm *gzvm, struct gzvm_dtb_config *cfg)
{
	struct arm_smccc_res res;

	return gzvm_hypcall_wrapper(MT_HVC_GZVM_SET_DTB_CONFIG, gzvm->vm_id,
				    cfg->dtb_addr, cfg->dtb_size, 0, 0, 0, 0,
				    &res);
}

static int gzvm_vm_arch_enable_cap(struct gzvm *gzvm,
				   struct gzvm_enable_cap *cap,
				   struct arm_smccc_res *res)
{
	return gzvm_hypcall_wrapper(MT_HVC_GZVM_ENABLE_CAP, gzvm->vm_id,
				    cap->cap, cap->args[0], cap->args[1],
				    cap->args[2], cap->args[3], cap->args[4],
				    res);
}

int gzvm_vm_ioctl_arch_enable_cap(struct gzvm *gzvm,
				  struct gzvm_enable_cap *cap,
				  void __user *argp)
{
	struct arm_smccc_res res = {0};
	int ret;

	switch (cap->cap) {
	case GZVM_CAP_PROTECTED_VM:
		return -EOPNOTSUPP;
	case GZVM_CAP_BLOCK_BASED_DEMAND_PAGING:
		ret = gzvm_vm_arch_enable_cap(gzvm, cap, &res);
		return ret;
	default:
		break;
	}

	return -EINVAL;
}

/**
 * gzvm_hva_to_pa_arch() - converts hva to pa with arch-specific way
 * @hva: Host virtual address.
 *
 * Return: GZVM_PA_ERR_BAD for translation error
 */
u64 gzvm_hva_to_pa_arch(u64 hva)
{
	unsigned long flags;
	u64 par;

	local_irq_save(flags);
	asm volatile("at s1e1r, %0" :: "r" (hva));
	isb();
	par = read_sysreg_par();
	local_irq_restore(flags);

	if (par & SYS_PAR_EL1_F)
		return GZVM_PA_ERR_BAD;
	par = par & PAR_PA47_MASK;
	if (!par)
		return GZVM_PA_ERR_BAD;
	return par;
}

int gzvm_arch_map_guest(u16 vm_id, int memslot_id, u64 pfn, u64 gfn,
			u64 nr_pages)
{
	struct arm_smccc_res res;

	return gzvm_hypcall_wrapper(MT_HVC_GZVM_MAP_GUEST, vm_id, memslot_id,
				    pfn, gfn, nr_pages, 0, 0, &res);
}

int gzvm_arch_map_guest_block(u16 vm_id, int memslot_id, u64 gfn, u64 nr_pages)
{
	struct arm_smccc_res res;

	return gzvm_hypcall_wrapper(MT_HVC_GZVM_MAP_GUEST_BLOCK, vm_id,
				    memslot_id, gfn, nr_pages, 0, 0, 0, &res);
}
