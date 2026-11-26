// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/irqchip/arm-gic-v3.h>
#include <linux/ratelimit.h>
#include <linux/gzvm.h>
#include "gzvm_drv_v9_1.h"
#include "gzvm_arch_common.h"

/*
 * Early GenieZone firmware takes the architectural INTID in IRQ_LINE.
 * Later firmware takes the SPI index and adds the private-interrupt offset.
 */
#define GZVM_SPI_INTID_BASE 32U
#define GZVM_SPI_INTID_MAX 1019U

int gzvm_arch_create_device(u16 vm_id, struct gzvm_create_device *gzvm_dev)
{
	struct arm_smccc_res res;

	return gzvm_hypcall_wrapper(MT_HVC_GZVM_CREATE_DEVICE, vm_id,
				    virt_to_phys(gzvm_dev), 0, 0, 0, 0, 0,
				    &res);
}

static int gzvm_inject_irq_line(struct gzvm *gzvm, unsigned int vcpu_idx,
				u32 hvc_irq, bool level)
{
	unsigned long tuple = assemble_vm_vcpu_tuple(gzvm->vm_id, vcpu_idx);
	struct arm_smccc_res res;

	gzvm_hypcall_wrapper(MT_HVC_GZVM_IRQ_LINE, tuple, hvc_irq, level,
			    0, 0, 0, 0, &res);
	if (res.a0) {
		pr_err_ratelimited("gzvm: IRQ_LINE vm=%u vcpu=%u irq=%u level=%d failed (%ld)\n",
				   gzvm->vm_id, vcpu_idx, hvc_irq, level, (long)res.a0);
		return -EFAULT;
	}

	return 0;
}

int gzvm_arch_inject_ppi(struct gzvm_vcpu *vcpu, u32 intid)
{
	return gzvm_inject_irq_line(vcpu->gzvm, vcpu->vcpuid, intid, true);
}

/**
 * gzvm_arch_inject_irq() - Inject virtual interrupt to a VM
 * @gzvm: Pointer to struct gzvm
 * @vcpu_idx: vcpu index, only valid if PPI
 * @irq: *SPI* irq number (excluding offset value `32`)
 * @level: 1 if true else 0
 *
 * Return:
 * * 0			- Success.
 * * Negative		- Failure.
 */
int gzvm_arch_inject_irq(struct gzvm *gzvm, unsigned int vcpu_idx,
			 u32 irq, bool level)
{
	u32 hvc_irq = irq;

	if (gzvm->quirks & GZVM_QUIRK_LEGACY_SPI_INTID) {
		if (irq > GZVM_SPI_INTID_MAX - GZVM_SPI_INTID_BASE)
			return -EINVAL;
		hvc_irq += GZVM_SPI_INTID_BASE;
	}

	return gzvm_inject_irq_line(gzvm, vcpu_idx, hvc_irq, level);
}
