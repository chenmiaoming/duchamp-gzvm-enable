// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <asm/arch_timer.h>
#include <linux/arm-smccc.h>
#include <linux/err.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/ratelimit.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include <linux/gzvm.h>
#include "gzvm_drv_v9_1.h"
#include "gzvm_arch_common.h"

/*
 * Temporary, read-only diagnosis of the 9.1 reserved[0] rejection.
 * Snapshot the shared union without modifying any MMIO/hypercall response.
 * This records evidence only: the exception policy and HVC result are unchanged.
 */
static bool r0trace;
module_param(r0trace, bool, 0444);
MODULE_PARM_DESC(r0trace, "r0trace1: log RUN input/output on unexpected reserved words");

static bool legacy_lr_trace;
module_param(legacy_lr_trace, bool, 0444);
MODULE_PARM_DESC(legacy_lr_trace, "Observe legacy firmware GIC list register state");

static enum hrtimer_restart gzvm_vtimer_expire(struct hrtimer *timer)
{
	struct gzvm_vcpu *vcpu = container_of(timer, struct gzvm_vcpu,
					     legacy_vtimer);

	if (!READ_ONCE(vcpu->gzvm->fatal) &&
	    !gzvm_arch_inject_ppi(vcpu, vcpu->legacy_vtimer_irq) &&
	    legacy_lr_trace)
		pr_info_once("gzvm: legacy virtual timer injected PPI %u\n",
			     vcpu->legacy_vtimer_irq);

	return HRTIMER_NORESTART;
}

void gzvm_arch_init_vcpu_timer(struct gzvm_vcpu *vcpu)
{
	hrtimer_init(&vcpu->legacy_vtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	vcpu->legacy_vtimer.function = gzvm_vtimer_expire;
}

static void gzvm_arm_legacy_vtimer(struct gzvm_vcpu *vcpu)
{
	struct gzvm_vcpu_hwstate *state = vcpu->hwstate;
	u32 intid, rate, remainder;
	u64 seconds, ns;

	if (!READ_ONCE(state->vtimer_migrate))
		return;

	intid = le32_to_cpu(READ_ONCE(state->vtimer_irq_num));
	rate = arch_timer_get_cntfrq();
	if (intid < 16 || intid >= 32 || !rate)
		return;

	seconds = div_u64_rem(le64_to_cpu(READ_ONCE(state->vtimer_delay)),
			      rate, &remainder);
	if (seconds >= KTIME_SEC_MAX)
		return;
	ns = div_u64((u64)remainder * NSEC_PER_SEC, rate);
	vcpu->legacy_vtimer_irq = intid;
	hrtimer_start(&vcpu->legacy_vtimer, ktime_set(seconds, ns),
		      HRTIMER_MODE_REL_HARD);
}

static void gzvm_trace_list_registers(struct gzvm_vcpu *vcpu)
{
	struct gzvm_vcpu_hwstate *state = vcpu->hwstate;
	u32 count = le32_to_cpu(READ_ONCE(state->nr_lrs));
	u32 i;

	pr_info_once("gzvm: legacy LR first RUN: count=%u\n", count);
	if (!count || count > ARRAY_SIZE(state->lr))
		return;

	pr_info_once("gzvm: legacy firmware published %u LRs\n", count);
	for (i = 0; i < count; i++) {
		u64 lr = le64_to_cpu(READ_ONCE(state->lr[i]));
		u32 intid = lr & ICH_LR_VIRTUAL_ID_MASK;

		if (intid >= 32 && intid <= 1019 &&
		    (lr & ICH_LR_EOI) && !(lr & (ICH_LR_STATE | ICH_LR_HW)))
			pr_info_ratelimited("gzvm: legacy software SPI EOI vm=%u vcpu=%d intid=%u\n",
					    vcpu->gzvm->vm_id, vcpu->vcpuid, intid);
	}
}

static void gzvm_trace_reserved(struct gzvm_vcpu *vcpu,
			       const struct gzvm_vcpu_run *before,
			       const struct arm_smccc_res *res, int ret)
{
	static DEFINE_RATELIMIT_STATE(trace_rs, 5 * HZ, 2);
	struct gzvm_vcpu_run after;
	int i;
	bool unexpected = false;

	if (res->a1 != GZVM_EXIT_EXCEPTION)
		return;
	memcpy(&after, vcpu->run, sizeof(after));
	for (i = 0; i < ARRAY_SIZE(after.exception.reserved); i++) {
		/* Word 6 carries the already observed legacy ESR_EL2. */
		if (i != 6 && after.exception.reserved[i]) {
			unexpected = true;
			break;
		}
	}
	if (!unexpected || !__ratelimit(&trace_rs))
		return;

	pr_warn("gzvm_r0trace1: vm=%u vcpu=%d ret=%d raw_a0=0x%lx raw_a1=0x%lx input_exit=0x%x\n",
		vcpu->gzvm->vm_id, vcpu->vcpuid, ret,
		res->a0, res->a1, before->exit_reason);
	pr_warn("gzvm_r0trace1: output exit=0x%x type=%u error=0x%x gpa=0x%llx esr=0x%llx\n",
		after.exit_reason, after.exception.exception,
		after.exception.error_code,
		(unsigned long long)after.exception.fault_gpa,
		(unsigned long long)after.exception.reserved[6]);
	if (before->exit_reason == GZVM_EXIT_MMIO)
		pr_warn("gzvm_r0trace1: input MMIO addr=0x%llx size=%llu reg=%u write=%u data=%*phN\n",
			(unsigned long long)before->mmio.phys_addr,
			(unsigned long long)before->mmio.size,
			before->mmio.reg_nr, before->mmio.is_write,
			(int)sizeof(before->mmio.data), before->mmio.data);
	if (before->exit_reason == GZVM_EXIT_HYPERCALL)
		pr_warn("gzvm_r0trace1: input HVC arg0=0x%llx arg1=0x%llx arg2=0x%llx\n",
			(unsigned long long)before->hypercall.args[0],
			(unsigned long long)before->hypercall.args[1],
			(unsigned long long)before->hypercall.args[2]);
	for (i = 0; i < ARRAY_SIZE(after.exception.reserved); i++) {
		u64 in = before->exception.reserved[i];
		u64 out = after.exception.reserved[i];

		if (in || out)
			pr_warn("gzvm_r0trace1: word[%d] in=0x%llx out=0x%llx %s\n",
				i, (unsigned long long)in, (unsigned long long)out,
				in == out ? "same-value" : "changed");
	}
}

int gzvm_arch_vcpu_update_one_reg(struct gzvm_vcpu *vcpu, __u64 reg_id,
				  bool is_write, __u64 *data)
{
	struct arm_smccc_res res;
	unsigned long a1;
	int ret;

	a1 = assemble_vm_vcpu_tuple(vcpu->gzvm->vm_id, vcpu->vcpuid);
	if (!is_write) {
		ret = gzvm_hypcall_wrapper(MT_HVC_GZVM_GET_ONE_REG,
					   a1, reg_id, 0, 0, 0, 0, 0, &res);
		if (ret == 0)
			*data = res.a1;
	} else {
		ret = gzvm_hypcall_wrapper(MT_HVC_GZVM_SET_ONE_REG,
					   a1, reg_id, *data, 0, 0, 0, 0, &res);
	}

	return ret;
}

int gzvm_arch_vcpu_run(struct gzvm_vcpu *vcpu, __u64 *exit_reason)
{
	struct gzvm_vcpu_hwstate *state = vcpu->hwstate;
	bool legacy_vtimer = vcpu->gzvm->quirks & GZVM_QUIRK_LEGACY_VTIMER;
	struct gzvm_vcpu_run before;
	struct arm_smccc_res res;
	unsigned long a1;
	int ret;

	if (legacy_vtimer && READ_ONCE(state->vtimer_migrate)) {
		hrtimer_cancel(&vcpu->legacy_vtimer);
		WRITE_ONCE(state->vtimer_migrate, 0);
		WRITE_ONCE(state->vtimer_delay, 0);
	}
	if (r0trace)
		memcpy(&before, vcpu->run, sizeof(before));
	a1 = assemble_vm_vcpu_tuple(vcpu->gzvm->vm_id, vcpu->vcpuid);
	ret = gzvm_hypcall_wrapper(MT_HVC_GZVM_RUN, a1, 0, 0, 0, 0, 0,
				   0, &res);
	if (legacy_vtimer && !ret)
		gzvm_arm_legacy_vtimer(vcpu);
	*exit_reason = res.a1;
	if (legacy_lr_trace && !ret)
		gzvm_trace_list_registers(vcpu);
	if (r0trace)
		gzvm_trace_reserved(vcpu, &before, &res, ret);
	return ret;
}

int gzvm_arch_destroy_vcpu(u16 vm_id, int vcpuid)
{
	struct arm_smccc_res res;
	unsigned long a1;

	a1 = assemble_vm_vcpu_tuple(vm_id, vcpuid);
	return gzvm_hypcall_wrapper(MT_HVC_GZVM_DESTROY_VCPU, a1, 0, 0, 0,
				    0, 0, 0, &res);
}

/**
 * gzvm_arch_create_vcpu() - Call smc to gz hypervisor to create vcpu
 * @vm_id: vm id
 * @vcpuid: vcpu id
 * @run: Virtual address of vcpu->run
 *
 * Return: The wrapper helps caller to convert geniezone errno to Linux errno.
 */
int gzvm_arch_create_vcpu(u16 vm_id, int vcpuid, void *run)
{
	struct arm_smccc_res res;
	unsigned long a1, a2;
	int ret;

	a1 = assemble_vm_vcpu_tuple(vm_id, vcpuid);
	a2 = (__u64)virt_to_phys(run);
	ret = gzvm_hypcall_wrapper(MT_HVC_GZVM_CREATE_VCPU, a1, a2, 0, 0, 0, 0,
				   0, &res);

	return ret;
}
