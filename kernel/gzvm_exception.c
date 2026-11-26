// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 * Local v9.1 adaptation for the 6.1 GenieZone host driver.
 */

#include <linux/ratelimit.h>

#include "gzvm_drv_v9_1.h"

#define GZVM_REPEAT_FAULT_LIMIT 64

/*
 * The original MediaTek v5 host ignored the six words preceding arch.esr_el2.
 * Although its UAPI documented these as zero, the old handler did not enforce
 * that rule.  Opt in to that old-host tolerance only for a validated lower-EL
 * translation fault with error_code == 0.  This does not prove the firmware's
 * meaning of nonzero words.  Never clear them before RUN: MMIO and hypercall
 * responses overlap this shared union.
 */
bool gzvm_handle_guest_exception(struct gzvm_vcpu *vcpu)
{
	struct gzvm_vcpu_run *run = vcpu->run;
	u32 quirks = vcpu->gzvm->quirks;
	u64 esr = READ_ONCE(run->exception.reserved[6]);
	u64 gpa = READ_ONCE(run->exception.fault_gpa);
	u64 page_gpa = gpa & PAGE_MASK;
	u32 ec = (esr >> 26) & 0x3f;
	u32 fsc = esr & 0x3f;
	bool translation_fault;
	bool legacy_padding, nonzero_padding = false;
	u64 n;
	int i, ret;

	/*
	 * Only accept the observed old-ABI field for a lower-EL instruction/data
	 * translation fault.  EC, IL and FSC are checked; the data-abort ISS
	 * contains valid additional bits.  The optional six-word compatibility
	 * below restores old-host tolerance, not a new firmware flag definition.
	 */
	translation_fault = (quirks & GZVM_QUIRK_LEGACY_ESR) &&
		run->exception.exception == GZVM_EXCEPTION_PAGE_FAULT &&
		!(esr >> 32) && (esr & BIT_ULL(25)) &&
		(ec == 0x20 || ec == 0x24) && fsc >= 4 && fsc <= 7;
	legacy_padding = (quirks & GZVM_QUIRK_LEGACY_RESERVED) && translation_fault &&
		READ_ONCE(run->exception.error_code) == 0;
	for (i = 0; i < ARRAY_SIZE(run->exception.reserved); i++) {
		u64 value = READ_ONCE(run->exception.reserved[i]);

		if (!value || (i == 6 && translation_fault))
			continue;
		if (i < 6 && legacy_padding) {
			nonzero_padding = true;
			continue;
		}
		pr_warn_ratelimited("gzvm_v9_1: reserved[%d]=0x%llx rejected type=%u error=0x%x gpa=0x%llx esr=0x%llx\n",
				    i, (unsigned long long)value,
				    run->exception.exception,
				    run->exception.error_code,
				    (unsigned long long)gpa,
				    (unsigned long long)esr);
		return false;
	}
	if (nonzero_padding)
		pr_warn_once("gzvm_r0compat1: legacy six-word padding accepted vm=%u vcpu=%d type=%u error=0x%x gpa=0x%llx esr=0x%llx; page validation still required\n",
			     vcpu->gzvm->vm_id, vcpu->vcpuid,
			     run->exception.exception, run->exception.error_code,
			     (unsigned long long)gpa, (unsigned long long)esr);

	if (run->exception.exception != GZVM_EXCEPTION_PAGE_FAULT)
		return false;

	/*
	 * The previous total-fault budget could terminate a normal large guest.
	 * Only consecutive retries of one guest page are bounded here.  Compare
	 * page numbers, because offsets within one page do not show map progress.
	 * This is a diagnostic stop, not a proof of guest instruction progress.
	 */
	n = ++vcpu->diag_faults;
	if (vcpu->diag_same_gpa >= GZVM_REPEAT_FAULT_LIMIT &&
	    vcpu->diag_last_gpa == page_gpa) {
		pr_warn_ratelimited("gzvm_v9_1: repeat guard vm=%u vcpu=%d faults=%llu page=0x%llx repeats=%u\n",
				    vcpu->gzvm->vm_id, vcpu->vcpuid,
				    (unsigned long long)n,
				    (unsigned long long)page_gpa,
				    vcpu->diag_same_gpa);
		return false;
	}
	if (n <= 8 || !(n & (n - 1)))
		pr_info("gzvm_v9_1: fault=%llu vm=%u vcpu=%d esr=0x%llx gpa=0x%llx\n",
			(unsigned long long)n, vcpu->gzvm->vm_id, vcpu->vcpuid,
			(unsigned long long)esr, (unsigned long long)gpa);

	ret = gzvm_handle_page_fault(vcpu);
	if (ret) {
		pr_warn_ratelimited("gzvm_v9_1: page fault failed vm=%u vcpu=%d gpa=0x%llx ret=%d\n",
				    vcpu->gzvm->vm_id, vcpu->vcpuid,
				    (unsigned long long)gpa, ret);
		return false;
	}
	if (vcpu->diag_same_gpa && vcpu->diag_last_gpa == page_gpa)
		vcpu->diag_same_gpa++;
	else {
		vcpu->diag_last_gpa = page_gpa;
		vcpu->diag_same_gpa = 1;
	}
	return true;
}

bool gzvm_handle_guest_hvc(struct gzvm_vcpu *vcpu)
{
	unsigned long func = READ_ONCE(vcpu->run->hypercall.args[0]);
	unsigned long ipa = READ_ONCE(vcpu->run->hypercall.args[1]);
	int ret;

	switch (func) {
	case GZVM_HVC_MEM_RELINQUISH:
		ret = gzvm_handle_relinquish(vcpu, ipa);
		return ret == 0;
	default:
		return false;
	}
}
