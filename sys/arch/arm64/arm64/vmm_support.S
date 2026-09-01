/*	$OpenBSD$ */
/*
 * Copyright (c) 2026 Dave Voutila <dv@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "assym.h"
#include <machine/asm.h>
#include <machine/armreg.h>

	.arch armv8.1-a
	.arch_extension fp
	.arch_extension pauth

#define VMM_HVC_ENTER	0x4f42

/*
 * Enter EL2 with x0 holding the physical address of struct arm64_vmm_run.
 * OpenBSD keeps its per-CPU pointer in the platform register x18.  Preserve
 * it along with the AAPCS64 callee-saved registers before the guest replaces
 * the complete integer register file.  EL2 restores this stack before
 * returning here.
 */
ENTRY(arm64_vmm_enter_nvhe)
	sub	sp, sp, #112
	str	x18, [sp, #0]
	stp	x19, x20, [sp, #16]
	stp	x21, x22, [sp, #32]
	stp	x23, x24, [sp, #48]
	stp	x25, x26, [sp, #64]
	stp	x27, x28, [sp, #80]
	stp	x29, x30, [sp, #96]

	hvc	#VMM_HVC_ENTER

	ldr	x18, [sp, #0]
	ldp	x19, x20, [sp, #16]
	ldp	x21, x22, [sp, #32]
	ldp	x23, x24, [sp, #48]
	ldp	x25, x26, [sp, #64]
	ldp	x27, x28, [sp, #80]
	ldp	x29, x30, [sp, #96]
	add	sp, sp, #112
	ret
END(arm64_vmm_enter_nvhe)

/*
 * Enter a guest from a VHE host.  HCR_EL2.E2H remains set, so the host's EL2
 * stage-1 translation regime stays active and the run-page pointer is a
 * kernel virtual address in this mode.  x1 selects a full entry (zero), an
 * immediate same-call retry (one), or reuse of a resident EL12 bank with a
 * fresh host snapshot (two).  x2 is non-zero when an
 * asynchronous exit must snapshot the resident EL12 bank before returning
 * to userland.  A non-zero x3 says that C has masked and saved the VHE host's
 * hyp-virtual timer for a short recovery entry.
 */
ENTRY(arm64_vmm_enter_vhe)
	sub	sp, sp, #112
	str	x18, [sp, #0]
	stp	x19, x20, [sp, #16]
	stp	x21, x22, [sp, #32]
	stp	x23, x24, [sp, #48]
	stp	x25, x26, [sp, #64]
	stp	x27, x28, [sp, #80]
	stp	x29, x30, [sp, #96]

	/*
	 * Mask exceptions while the host snapshot and vmm vectors are only
	 * partially installed.  The original DAIF value is saved in the host
	 * PSTATE below and restored by the final ERET.
	 */
	mov	x14, x2
	mrs	x2, daif
	msr	daifset, #0xf
	mov	x15, x1
	mov	x17, x0
	msr	tpidr_el2, x17
	mov	x0, #ARM64_VMM_MODE_VHE
	str	x0, [x17, #AVR_MODE]
	str	x14, [x17, #AVR_HOST_ESR_EL1]
	str	x3, [x17, #AVR_DEFER_CNTHV]

	/*
	 * Mode one may reuse the host snapshot made by the immediately preceding
	 * entry in this call.  Mode two reuses only the resident guest EL12 bank;
	 * it must snapshot the new syscall's stack and host state below.  TPIDR_EL1
	 * is curcpu() on arm64.  A mismatch invalidates either reuse mode and turns
	 * it into a full entry.  This check is done after masking exceptions so the
	 * CPU cannot change between the comparison and the guest entry sequence.
	 */
	cbz	x15, .Lvhe_save_host
	mrs	x0, tpidr_el1
	ldr	x1, [x17, #AVR_HOST_TPIDR_EL1]
	cmp	x0, x1
	b.ne	.Lvhe_reuse_stale
	cmp	x15, #1
	b.eq	.Lvhe_host_saved
	b	.Lvhe_save_host
.Lvhe_reuse_stale:
	mov	x15, xzr

.Lvhe_save_host:
	/* Save state which guest execution or the EL2 vectors overwrite. */
	mov	x0, sp
	str	x0, [x17, #AVR_HOST_SP]
	mrs	x0, sp_el0
	str	x0, [x17, #AVR_HOST_SP_EL0]
	adr	x0, .Lvhe_return
	str	x0, [x17, #AVR_HOST_PC]
	mrs	x1, nzcv
	orr	x0, x2, x1
	mov	x1, #PSR_M_EL2h
	orr	x1, x1, #PSR_PAN
	orr	x0, x0, x1
	str	x0, [x17, #AVR_HOST_PSTATE]
	mrs	x0, vbar_el2
	str	x0, [x17, #AVR_HOST_VBAR_EL1]
	mrs	x0, contextidr_el1
	str	x0, [x17, #AVR_HOST_CONTEXTIDR_EL1]
	mrs	x0, cpacr_el1
	str	x0, [x17, #AVR_HOST_CPACR_EL1]
	mrs	x0, cntkctl_el1
	str	x0, [x17, #AVR_HOST_CNTKCTL_EL1]
	mrs	x0, tpidr_el0
	str	x0, [x17, #AVR_HOST_TPIDR_EL0]
	mrs	x0, tpidrro_el0
	str	x0, [x17, #AVR_HOST_TPIDRRO_EL0]
	mrs	x0, tpidr_el1
	str	x0, [x17, #AVR_HOST_TPIDR_EL1]
	mrs	x0, cnthctl_el2
	str	x0, [x17, #AVR_HOST_CNTHCTL_EL2]
	mrs	x0, vmpidr_el2
	str	x0, [x17, #AVR_HOST_VMPIDR_EL2]
	mrs	x0, hcr_el2
	str	x0, [x17, #AVR_HOST_HCR_EL2]
	/* ICH_HCR_EL2 is per-CPU EL2 state, not part of the guest EL1 bank. */
	mrs	x0, ICH_HCR_EL2
	str	x0, [x17, #AVR_HOST_ICH_HCR_EL2]
	mrs	x0, ICH_VMCR_EL2
	str	x0, [x17, #AVR_HOST_ICH_VMCR_EL2]
	mrs	x0, ICH_LR0_EL2
	str	x0, [x17, #AVR_HOST_ICH_LR0_EL2]
	/*
	 * API/APK let the guest execute pointer-authentication instructions and
	 * write the key registers directly.  The keys are not banked, so save the
	 * host process's keys before HCR exposes them to the guest.
	 */
	add	x16, x17, #AVR_HOST_PAUTH
	mrs	x0, apiakeylo_el1
	mrs	x1, apiakeyhi_el1
	stp	x0, x1, [x16, #0]
	mrs	x0, apibkeylo_el1
	mrs	x1, apibkeyhi_el1
	stp	x0, x1, [x16, #16]
	mrs	x0, apdakeylo_el1
	mrs	x1, apdakeyhi_el1
	stp	x0, x1, [x16, #32]
	mrs	x0, apdbkeylo_el1
	mrs	x1, apdbkeyhi_el1
	stp	x0, x1, [x16, #48]
	mrs	x0, apgakeylo_el1
	mrs	x1, apgakeyhi_el1
	stp	x0, x1, [x16, #64]

.Lvhe_host_saved:
	/* All lower-EL AArch64 exceptions now enter the vmm vector slots. */
	adrp	x0, hyp_vectors
	add	x0, x0, :lo12:hyp_vectors
	msr	vbar_el2, x0
	/*
	 * VTTBR_EL2 and VTCR_EL2 are not restored for the VHE host.  A same-CPU
	 * retry therefore retains the VM's stage-2 context and can avoid two
	 * particularly expensive nested-virtualization traps.  A full entry,
	 * including the first entry after migration, always installs both.
	 */
	cbnz	x15, .Lvhe_stage2_ready
	ldr	x0, [x17, #AVR_VTTBR_EL2]
	msr	vttbr_el2, x0
	ldr	x0, [x17, #AVR_VTCR_EL2]
	msr	vtcr_el2, x0
.Lvhe_stage2_ready:
	/*
	 * Install the guest's hardware CPU-interface state while it is disabled.
	 * LR0 carries the real INTID, priority, and pending/active lifecycle, so
	 * guest IAR and EOIR instructions complete without trapping to software.
	 */
	msr	ICH_HCR_EL2, xzr
	ldr	x0, [x17, #AVR_ICH_VMCR_EL2]
	msr	ICH_VMCR_EL2, x0
	ldr	x0, [x17, #AVR_ICH_LR0_EL2]
	msr	ICH_LR0_EL2, x0
	ldr	x0, [x17, #AVR_ICH_HCR_EL2]
	msr	ICH_HCR_EL2, x0

	/*
	 * Clear TGE but retain E2H.  With E2H=1,TGE=0, EL12 names the
	 * guest's virtual EL1 bank while the unqualified EL1 registers still
	 * belong to the VHE host.  IMO/FMO/AMO also begin routing physical
	 * asynchronous exceptions to these EL2 vectors at this point.
	 */
	ldr	x0, [x17, #AVR_HCR_EL2]
	msr	hcr_el2, x0
	isb
	/*
	 * The first arm64 machine model exposes only the physical timer.  Keep
	 * the VHE guest's separate virtual timer output masked so its PPI cannot
	 * remain asserted while the host rearms the hyp-virtual tick.  EL1TVT in
	 * CNTHCTL_EL2 makes guest accesses trap instead of undoing this policy.
	 * s3_5_c14_c3_1 is CNTV_CTL_EL02, the VHE alias for the guest bank.
	 */
	mov	x0, #CNTV_CTL_IMASK
	msr	s3_5_c14_c3_1, x0
	isb
	ldr	x0, [x17, #AVR_FLUSH_TLB]
	cbz	x0, .Lvhe_tlb_done
	/*
	 * Value one is the full combined invalidation required after a stage-2
	 * mapping change or stale nested-shadow detection.  Larger values replay
	 * a trapped guest stage-1 TLBI after selecting its VTTBR.  Preserve the
	 * operand so an outer hypervisor can associate VA and ASID with its shadow.
	 */
	dsb	ishst
	cmp	x0, #2
	b.eq	.Lvhe_tlbi_vmalle1
	ldr	x1, [x17, #AVR_TLBI_ARG]
	cmp	x0, #3
	b.eq	.Lvhe_tlbi_vae1
	cmp	x0, #4
	b.eq	.Lvhe_tlbi_vaae1
	cmp	x0, #5
	b.eq	.Lvhe_tlbi_aside1
	tlbi	vmalls12e1is
	b	.Lvhe_tlbi_barrier
.Lvhe_tlbi_vmalle1:
	tlbi	vmalle1is
	b	.Lvhe_tlbi_barrier
.Lvhe_tlbi_vae1:
	tlbi	vae1is, x1
	b	.Lvhe_tlbi_barrier
.Lvhe_tlbi_vaae1:
	tlbi	vaae1is, x1
	b	.Lvhe_tlbi_barrier
.Lvhe_tlbi_aside1:
	tlbi	aside1is, x1
.Lvhe_tlbi_barrier:
	dsb	ish
	isb
.Lvhe_tlb_done:

	/*
	 * A physical interrupt does not alter the guest's banked EL12 state.
	 * The immediate retry remains in the same vm_run() invocation on the
	 * same host CPU, so it can reuse that state and avoid a long sequence of
	 * nested system-register writes.  Every entry from userland takes the
	 * full path, as does any entry whose state userland could have changed.
	 */
	cbnz	x15, .Lvhe_el12_done

	/* Restore guest EL1 state through the VHE EL12 aliases. */
	ldr	x0, [x17, #AVR_SCTLR_EL1]
	msr	sctlr_el12, x0
	isb
	ldr	x0, [x17, #AVR_TCR_EL1]
	msr	tcr_el12, x0
	ldr	x0, [x17, #AVR_TTBR0_EL1]
	msr	ttbr0_el12, x0
	ldr	x0, [x17, #AVR_TTBR1_EL1]
	msr	ttbr1_el12, x0
	ldr	x0, [x17, #AVR_MAIR_EL1]
	msr	mair_el12, x0
	ldr	x0, [x17, #AVR_VBAR_EL1]
	msr	vbar_el12, x0
	ldr	x0, [x17, #AVR_CONTEXTIDR_EL1]
	msr	contextidr_el12, x0
	ldr	x0, [x17, #AVR_CPACR_EL1]
	msr	cpacr_el12, x0
	ldr	x0, [x17, #AVR_CNTKCTL_EL1]
	msr	cntkctl_el12, x0
	ldr	x0, [x17, #AVR_ELR_EL1]
	msr	elr_el12, x0
	ldr	x0, [x17, #AVR_SPSR_EL1]
	msr	spsr_el12, x0
	ldr	x0, [x17, #AVR_ESR_EL1]
	msr	esr_el12, x0
	ldr	x0, [x17, #AVR_FAR_EL1]
	msr	far_el12, x0
.Lvhe_el12_done:
	/* EL0 and EL2 controls are shared with the VHE host and always reload. */
	ldr	x0, [x17, #AVR_SP_EL0]
	msr	sp_el0, x0
	ldr	x0, [x17, #AVR_TPIDR_EL0]
	msr	tpidr_el0, x0
	ldr	x0, [x17, #AVR_TPIDRRO_EL0]
	msr	tpidrro_el0, x0
	ldr	x0, [x17, #AVR_TPIDR_EL1]
	msr	tpidr_el1, x0
	/*
	 * Keep the VHE host's CNTVOFF_EL2.  The guest's virtual counter then
	 * inherits any offset applied by an outer hypervisor; on bare metal the
	 * inherited value is normally zero.  Replacing it here would both expose
	 * the wrong time base to a nested guest and force an expensive L0 trap on
	 * every entry.
	 */
	ldr	x0, [x17, #AVR_CNTHCTL_EL2]
	msr	cnthctl_el2, x0
	ldr	x0, [x17, #AVR_VMPIDR_EL2]
	msr	vmpidr_el2, x0
	ldr	x0, [x17, #AVR_SP]
	msr	sp_el1, x0
	ldr	x0, [x17, #AVR_PC]
	msr	elr_el2, x0
	ldr	x0, [x17, #AVR_PSTATE]
	msr	spsr_el2, x0
	dsb	ish
	isb

	/* The vector file is not banked by exception level. */
	add	x16, x17, #AVR_FP
	ldp	q0, q1, [x16], #32
	ldp	q2, q3, [x16], #32
	ldp	q4, q5, [x16], #32
	ldp	q6, q7, [x16], #32
	ldp	q8, q9, [x16], #32
	ldp	q10, q11, [x16], #32
	ldp	q12, q13, [x16], #32
	ldp	q14, q15, [x16], #32
	ldp	q16, q17, [x16], #32
	ldp	q18, q19, [x16], #32
	ldp	q20, q21, [x16], #32
	ldp	q22, q23, [x16], #32
	ldp	q24, q25, [x16], #32
	ldp	q26, q27, [x16], #32
	ldp	q28, q29, [x16], #32
	ldp	q30, q31, [x16]
	ldr	x0, [x17, #AVR_FPCR]
	msr	fpcr, x0
	ldr	x0, [x17, #AVR_FPSR]
	msr	fpsr, x0
	/* Install every guest pointer-authentication key before its first PAC. */
	add	x16, x17, #AVR_PAUTH
	ldp	x0, x1, [x16, #0]
	msr	apiakeylo_el1, x0
	msr	apiakeyhi_el1, x1
	ldp	x0, x1, [x16, #16]
	msr	apibkeylo_el1, x0
	msr	apibkeyhi_el1, x1
	ldp	x0, x1, [x16, #32]
	msr	apdakeylo_el1, x0
	msr	apdakeyhi_el1, x1
	ldp	x0, x1, [x16, #48]
	msr	apdbkeylo_el1, x0
	msr	apdbkeyhi_el1, x1
	ldp	x0, x1, [x16, #64]
	msr	apgakeylo_el1, x0
	msr	apgakeyhi_el1, x1
	isb
	/*
	 * Guarantee a 250ms execution window.  Use the timer-relative TVAL view:
	 * when VHE itself is nested, deriving an absolute CNTHV_CVAL_EL2 from the
	 * CNTVCT_EL0 alias can apply the wrong outer-hypervisor counter offset.
	 * Deriving the relative quantum from CNTFRQ_EL0 remains independent of the
	 * platform frequency.  Unmasking is deliberately the last timer operation
	 * before ERET, so the outer GIC cannot latch the timer PPI during a slow
	 * nested entry.  The old absolute deadline is restored on every exit, so
	 * delayed OpenBSD clock work is dispatched immediately afterward.
	 */
	ldr	x0, [x17, #AVR_DEFER_CNTHV]
	cbz	x0, .Lvhe_cnthv_deferred
	mrs	x0, cntfrq_el0
	mov	x1, #4
	udiv	x0, x0, x1
	msr	s3_4_c14_c3_0, x0	/* CNTHV_TVAL_EL2 */

.Lvhe_unmask_cnthv:
	ldr	x0, [x17, #AVR_HOST_CNTHV_CTL_EL2]
	msr	s3_4_c14_c3_1, x0	/* CNTHV_CTL_EL2 */
	isb
.Lvhe_cnthv_deferred:

	/* x17 is the run-page pointer, so restore the guest's x17 last. */
	ldp	x0, x1, [x17, #AVR_GPRS + 0]
	ldp	x2, x3, [x17, #AVR_GPRS + 16]
	ldp	x4, x5, [x17, #AVR_GPRS + 32]
	ldp	x6, x7, [x17, #AVR_GPRS + 48]
	ldp	x8, x9, [x17, #AVR_GPRS + 64]
	ldp	x10, x11, [x17, #AVR_GPRS + 80]
	ldp	x12, x13, [x17, #AVR_GPRS + 96]
	ldp	x14, x15, [x17, #AVR_GPRS + 112]
	ldr	x16, [x17, #AVR_GPRS + 128]
	ldr	x18, [x17, #AVR_GPRS + 144]
	ldp	x19, x20, [x17, #AVR_GPRS + 152]
	ldp	x21, x22, [x17, #AVR_GPRS + 168]
	ldp	x23, x24, [x17, #AVR_GPRS + 184]
	ldp	x25, x26, [x17, #AVR_GPRS + 200]
	ldp	x27, x28, [x17, #AVR_GPRS + 216]
	ldp	x29, x30, [x17, #AVR_GPRS + 232]
	ldr	x17, [x17, #AVR_GPRS + 136]
	eret

.Lvhe_return:
	/* EL2 restored the stack and branch target; finish the C ABI return. */
	ldr	x18, [sp, #0]
	ldp	x19, x20, [sp, #16]
	ldp	x21, x22, [sp, #32]
	ldp	x23, x24, [sp, #48]
	ldp	x25, x26, [sp, #64]
	ldp	x27, x28, [sp, #80]
	ldp	x29, x30, [sp, #96]
	add	sp, sp, #112
	ret
END(arm64_vmm_enter_vhe)

/*
 * EL2 synchronous vector.  tpidr_el2 is both the guest-active sentinel and
 * the run-page pointer: zero identifies the private host HVC entry, while a
 * non-zero value identifies an exception taken from the guest.
 */
ENTRY_NP(vmm_hyp_sync)
	sub	sp, sp, #32
	stp	x16, x17, [sp]
	str	x18, [sp, #16]
	mrs	x17, tpidr_el2
	cbnz	x17, .Lguest_sync
	ldp	x16, x17, [sp]
	add	sp, sp, #32

	/* Only the private host-entry HVC is accepted outside guest context. */
	mrs	x16, esr_el2
	lsr	x15, x16, #26
	cmp	x15, #0x16		/* HVC from AArch64 */
	b.ne	.Lbad_hyp
	and	x16, x16, #0xffff
	mov	x15, #VMM_HVC_ENTER
	cmp	x16, x15
	b.ne	.Lbad_hyp

	/* x0 is safe to dereference here: nVHE EL2 uses physical addressing. */
	mov	x17, x0
	msr	tpidr_el2, x17
	mov	x0, #ARM64_VMM_MODE_NVHE
	str	x0, [x17, #AVR_MODE]

	/*
	 * nVHE runs the host at EL1 and the guest at the same exception level.
	 * Save every EL1/EL0 register that will be replaced with guest state.
	 */
	mrs	x0, sp_el1
	str	x0, [x17, #AVR_HOST_SP]
	mrs	x0, sp_el0
	str	x0, [x17, #AVR_HOST_SP_EL0]
	mrs	x0, elr_el2
	str	x0, [x17, #AVR_HOST_PC]
	mrs	x0, spsr_el2
	str	x0, [x17, #AVR_HOST_PSTATE]
	mrs	x0, elr_el1
	str	x0, [x17, #AVR_HOST_ELR_EL1]
	mrs	x0, spsr_el1
	str	x0, [x17, #AVR_HOST_SPSR_EL1]
	mrs	x0, esr_el1
	str	x0, [x17, #AVR_HOST_ESR_EL1]
	mrs	x0, far_el1
	str	x0, [x17, #AVR_HOST_FAR_EL1]
	mrs	x0, sctlr_el1
	str	x0, [x17, #AVR_HOST_SCTLR_EL1]
	mrs	x0, tcr_el1
	str	x0, [x17, #AVR_HOST_TCR_EL1]
	mrs	x0, ttbr0_el1
	str	x0, [x17, #AVR_HOST_TTBR0_EL1]
	mrs	x0, ttbr1_el1
	str	x0, [x17, #AVR_HOST_TTBR1_EL1]
	mrs	x0, mair_el1
	str	x0, [x17, #AVR_HOST_MAIR_EL1]
	mrs	x0, vbar_el1
	str	x0, [x17, #AVR_HOST_VBAR_EL1]
	mrs	x0, contextidr_el1
	str	x0, [x17, #AVR_HOST_CONTEXTIDR_EL1]
	mrs	x0, cpacr_el1
	str	x0, [x17, #AVR_HOST_CPACR_EL1]
	mrs	x0, cntkctl_el1
	str	x0, [x17, #AVR_HOST_CNTKCTL_EL1]
	mrs	x0, tpidr_el0
	str	x0, [x17, #AVR_HOST_TPIDR_EL0]
	mrs	x0, tpidrro_el0
	str	x0, [x17, #AVR_HOST_TPIDRRO_EL0]
	mrs	x0, tpidr_el1
	str	x0, [x17, #AVR_HOST_TPIDR_EL1]
	mrs	x0, cntvoff_el2
	str	x0, [x17, #AVR_HOST_CNTVOFF_EL2]
	mrs	x0, cnthctl_el2
	str	x0, [x17, #AVR_HOST_CNTHCTL_EL2]
	mrs	x0, vmpidr_el2
	str	x0, [x17, #AVR_HOST_VMPIDR_EL2]
	mrs	x0, hcr_el2
	str	x0, [x17, #AVR_HOST_HCR_EL2]
	/* Preserve the outer host's virtual-interface policy across nesting. */
	mrs	x0, ICH_HCR_EL2
	str	x0, [x17, #AVR_HOST_ICH_HCR_EL2]
	mrs	x0, ICH_VMCR_EL2
	str	x0, [x17, #AVR_HOST_ICH_VMCR_EL2]
	mrs	x0, ICH_LR0_EL2
	str	x0, [x17, #AVR_HOST_ICH_LR0_EL2]
	/* Save the nVHE host's five 128-bit pointer-authentication keys. */
	add	x16, x17, #AVR_HOST_PAUTH
	mrs	x0, apiakeylo_el1
	mrs	x1, apiakeyhi_el1
	stp	x0, x1, [x16, #0]
	mrs	x0, apibkeylo_el1
	mrs	x1, apibkeyhi_el1
	stp	x0, x1, [x16, #16]
	mrs	x0, apdakeylo_el1
	mrs	x1, apdakeyhi_el1
	stp	x0, x1, [x16, #32]
	mrs	x0, apdbkeylo_el1
	mrs	x1, apdbkeyhi_el1
	stp	x0, x1, [x16, #48]
	mrs	x0, apgakeylo_el1
	mrs	x1, apgakeyhi_el1
	stp	x0, x1, [x16, #64]

	/* Install the guest's stage-2 root, VMID, and address-space controls. */
	ldr	x0, [x17, #AVR_VTTBR_EL2]
	msr	vttbr_el2, x0
	ldr	x0, [x17, #AVR_VTCR_EL2]
	msr	vtcr_el2, x0
	/* See the VHE path: LR0 carries identity and its hardware lifecycle. */
	msr	ICH_HCR_EL2, xzr
	ldr	x0, [x17, #AVR_ICH_VMCR_EL2]
	msr	ICH_VMCR_EL2, x0
	ldr	x0, [x17, #AVR_ICH_LR0_EL2]
	msr	ICH_LR0_EL2, x0
	ldr	x0, [x17, #AVR_ICH_HCR_EL2]
	msr	ICH_HCR_EL2, x0
	ldr	x0, [x17, #AVR_FLUSH_TLB]
	cbz	x0, .Lnvhe_tlb_done
	/* See the VHE path above for the operation encoding and ordering. */
	dsb	ishst
	cmp	x0, #2
	b.eq	.Lnvhe_tlbi_vmalle1
	ldr	x1, [x17, #AVR_TLBI_ARG]
	cmp	x0, #3
	b.eq	.Lnvhe_tlbi_vae1
	cmp	x0, #4
	b.eq	.Lnvhe_tlbi_vaae1
	cmp	x0, #5
	b.eq	.Lnvhe_tlbi_aside1
	tlbi	vmalls12e1is
	b	.Lnvhe_tlbi_barrier
.Lnvhe_tlbi_vmalle1:
	tlbi	vmalle1is
	b	.Lnvhe_tlbi_barrier
.Lnvhe_tlbi_vae1:
	tlbi	vae1is, x1
	b	.Lnvhe_tlbi_barrier
.Lnvhe_tlbi_vaae1:
	tlbi	vaae1is, x1
	b	.Lnvhe_tlbi_barrier
.Lnvhe_tlbi_aside1:
	tlbi	aside1is, x1
.Lnvhe_tlbi_barrier:
	dsb	ish
	isb
.Lnvhe_tlb_done:

	/*
	 * Restore the guest's direct EL1 bank.  SCTLR is written first so a
	 * disabled guest MMU remains disabled while TTBR/TCR are replaced.
	 */
	ldr	x0, [x17, #AVR_SCTLR_EL1]
	msr	sctlr_el1, x0
	isb
	ldr	x0, [x17, #AVR_TCR_EL1]
	msr	tcr_el1, x0
	ldr	x0, [x17, #AVR_TTBR0_EL1]
	msr	ttbr0_el1, x0
	ldr	x0, [x17, #AVR_TTBR1_EL1]
	msr	ttbr1_el1, x0
	ldr	x0, [x17, #AVR_MAIR_EL1]
	msr	mair_el1, x0
	ldr	x0, [x17, #AVR_VBAR_EL1]
	msr	vbar_el1, x0
	ldr	x0, [x17, #AVR_CONTEXTIDR_EL1]
	msr	contextidr_el1, x0
	ldr	x0, [x17, #AVR_CPACR_EL1]
	msr	cpacr_el1, x0
	ldr	x0, [x17, #AVR_CNTKCTL_EL1]
	msr	cntkctl_el1, x0
	ldr	x0, [x17, #AVR_ELR_EL1]
	msr	elr_el1, x0
	ldr	x0, [x17, #AVR_SPSR_EL1]
	msr	spsr_el1, x0
	ldr	x0, [x17, #AVR_ESR_EL1]
	msr	esr_el1, x0
	ldr	x0, [x17, #AVR_FAR_EL1]
	msr	far_el1, x0
	ldr	x0, [x17, #AVR_SP_EL0]
	msr	sp_el0, x0
	ldr	x0, [x17, #AVR_TPIDR_EL0]
	msr	tpidr_el0, x0
	ldr	x0, [x17, #AVR_TPIDRRO_EL0]
	msr	tpidrro_el0, x0
	ldr	x0, [x17, #AVR_TPIDR_EL1]
	msr	tpidr_el1, x0
	ldr	x0, [x17, #AVR_CNTVOFF_EL2]
	msr	cntvoff_el2, x0
	ldr	x0, [x17, #AVR_CNTHCTL_EL2]
	msr	cnthctl_el2, x0
	ldr	x0, [x17, #AVR_VMPIDR_EL2]
	msr	vmpidr_el2, x0

	ldr	x0, [x17, #AVR_SP]
	msr	sp_el1, x0
	ldr	x0, [x17, #AVR_PC]
	msr	elr_el2, x0
	ldr	x0, [x17, #AVR_PSTATE]
	msr	spsr_el2, x0
	/* Enable stage 2 and physical exception routing only after state is live. */
	ldr	x0, [x17, #AVR_HCR_EL2]
	msr	hcr_el2, x0
	isb
	dsb	ish
	isb

	/* FP/AdvSIMD registers are shared between the host and guest. */
	add	x16, x17, #AVR_FP
	ldp	q0, q1, [x16], #32
	ldp	q2, q3, [x16], #32
	ldp	q4, q5, [x16], #32
	ldp	q6, q7, [x16], #32
	ldp	q8, q9, [x16], #32
	ldp	q10, q11, [x16], #32
	ldp	q12, q13, [x16], #32
	ldp	q14, q15, [x16], #32
	ldp	q16, q17, [x16], #32
	ldp	q18, q19, [x16], #32
	ldp	q20, q21, [x16], #32
	ldp	q22, q23, [x16], #32
	ldp	q24, q25, [x16], #32
	ldp	q26, q27, [x16], #32
	ldp	q28, q29, [x16], #32
	ldp	q30, q31, [x16]
	ldr	x0, [x17, #AVR_FPCR]
	msr	fpcr, x0
	ldr	x0, [x17, #AVR_FPSR]
	msr	fpsr, x0
	add	x16, x17, #AVR_PAUTH
	ldp	x0, x1, [x16, #0]
	msr	apiakeylo_el1, x0
	msr	apiakeyhi_el1, x1
	ldp	x0, x1, [x16, #16]
	msr	apibkeylo_el1, x0
	msr	apibkeyhi_el1, x1
	ldp	x0, x1, [x16, #32]
	msr	apdakeylo_el1, x0
	msr	apdakeyhi_el1, x1
	ldp	x0, x1, [x16, #48]
	msr	apdbkeylo_el1, x0
	msr	apdbkeyhi_el1, x1
	ldp	x0, x1, [x16, #64]
	msr	apgakeylo_el1, x0
	msr	apgakeyhi_el1, x1
	isb

	/* x17 carries the run page until the guest's x17 is loaded last. */
	ldp	x0, x1, [x17, #AVR_GPRS + 0]
	ldp	x2, x3, [x17, #AVR_GPRS + 16]
	ldp	x4, x5, [x17, #AVR_GPRS + 32]
	ldp	x6, x7, [x17, #AVR_GPRS + 48]
	ldp	x8, x9, [x17, #AVR_GPRS + 64]
	ldp	x10, x11, [x17, #AVR_GPRS + 80]
	ldp	x12, x13, [x17, #AVR_GPRS + 96]
	ldp	x14, x15, [x17, #AVR_GPRS + 112]
	ldr	x16, [x17, #AVR_GPRS + 128]
	ldr	x18, [x17, #AVR_GPRS + 144]
	ldp	x19, x20, [x17, #AVR_GPRS + 152]
	ldp	x21, x22, [x17, #AVR_GPRS + 168]
	ldp	x23, x24, [x17, #AVR_GPRS + 184]
	ldp	x25, x26, [x17, #AVR_GPRS + 200]
	ldp	x27, x28, [x17, #AVR_GPRS + 216]
	ldp	x29, x30, [x17, #AVR_GPRS + 232]
	ldr	x17, [x17, #AVR_GPRS + 136]
	eret

.Lguest_sync:
	/* ESR_EL2 is decoded by arm64_vmm_save_run() and vm_run(). */
	mov	x16, #ARM64_VMM_EXIT_SYNC
	str	x16, [x17, #AVR_EXIT]
	b	.Lguest_exit

ENTRY_NP(vmm_hyp_irq)
	/* Do not acknowledge the IRQ here; the restored host kernel will. */
	sub	sp, sp, #32
	stp	x16, x17, [sp]
	str	x18, [sp, #16]
	mrs	x17, tpidr_el2
	cbz	x17, .Lbad_hyp
	/*
	 * Probe only an interrupted EL0 PC.  AT S12E0R explicitly uses the
	 * selected guest stage-1 and stage-2 regimes, unlike S1E0R under VHE.
	 * Preserve the host PAR_EL1 around the probe: it is shared EL2 state,
	 * while the guest sees its EL12 bank.
	 */
	str	xzr, [x17, #AVR_ASYNC_PAR]
	mrs	x16, elr_el2
	str	x16, [x17, #AVR_ASYNC_PC]
	mrs	x16, spsr_el2
	and	x16, x16, #0xf
	cbnz	x16, 1f
	mrs	x18, par_el1
	ldr	x16, [x17, #AVR_ASYNC_PC]
	at	s12e0r, x16
	isb
	mrs	x16, par_el1
	str	x16, [x17, #AVR_ASYNC_PAR]
	msr	par_el1, x18
1:
	mov	x16, #ARM64_VMM_EXIT_IRQ
	str	x16, [x17, #AVR_EXIT]
	b	.Lguest_exit
END(vmm_hyp_irq)

ENTRY_NP(vmm_hyp_fiq)
	/* FIQ follows the same transparent host-delivery path as IRQ. */
	sub	sp, sp, #32
	stp	x16, x17, [sp]
	str	x18, [sp, #16]
	mrs	x17, tpidr_el2
	cbz	x17, .Lbad_hyp
	/* Keep the FIQ vector's translation check identical to the IRQ path. */
	str	xzr, [x17, #AVR_ASYNC_PAR]
	mrs	x16, elr_el2
	str	x16, [x17, #AVR_ASYNC_PC]
	mrs	x16, spsr_el2
	and	x16, x16, #0xf
	cbnz	x16, 1f
	mrs	x18, par_el1
	ldr	x16, [x17, #AVR_ASYNC_PC]
	at	s12e0r, x16
	isb
	mrs	x16, par_el1
	str	x16, [x17, #AVR_ASYNC_PAR]
	msr	par_el1, x18
1:
	mov	x16, #ARM64_VMM_EXIT_FIQ
	str	x16, [x17, #AVR_EXIT]
	b	.Lguest_exit
END(vmm_hyp_fiq)

ENTRY_NP(vmm_hyp_serror)
	/* C currently treats a physical SError observed in a vCPU as fatal. */
	sub	sp, sp, #32
	stp	x16, x17, [sp]
	str	x18, [sp, #16]
	mrs	x17, tpidr_el2
	cbz	x17, .Lbad_hyp
	mov	x16, #ARM64_VMM_EXIT_SERROR
	str	x16, [x17, #AVR_EXIT]
END(vmm_hyp_serror)

.Lguest_exit:
	/*
	 * x16-x18 were saved by the vector stub and all other guest GPRs are
	 * still live.  Use x18 for the run-page base, then recover all three
	 * saved registers after x0-x15 no longer need to remain live.
	 */
	mrs	x18, tpidr_el2

	stp	x0, x1, [x18, #AVR_GPRS + 0]
	stp	x2, x3, [x18, #AVR_GPRS + 16]
	stp	x4, x5, [x18, #AVR_GPRS + 32]
	stp	x6, x7, [x18, #AVR_GPRS + 48]
	stp	x8, x9, [x18, #AVR_GPRS + 64]
	stp	x10, x11, [x18, #AVR_GPRS + 80]
	stp	x12, x13, [x18, #AVR_GPRS + 96]
	stp	x14, x15, [x18, #AVR_GPRS + 112]
	ldp	x16, x17, [sp]
	stp	x16, x17, [x18, #AVR_GPRS + 128]
	ldr	x0, [sp, #16]
	str	x0, [x18, #AVR_GPRS + 144]
	add	sp, sp, #32
	stp	x19, x20, [x18, #AVR_GPRS + 152]
	stp	x21, x22, [x18, #AVR_GPRS + 168]
	stp	x23, x24, [x18, #AVR_GPRS + 184]
	stp	x25, x26, [x18, #AVR_GPRS + 200]
	stp	x27, x28, [x18, #AVR_GPRS + 216]
	stp	x29, x30, [x18, #AVR_GPRS + 232]

	/*
	 * FP/AdvSIMD has no EL1/EL2 bank.  Capture it before restoring any host
	 * control state; the C wrapper arranged for the host process's previous
	 * vector state to be held in its PCB rather than in these registers.
	 */
	add	x16, x18, #AVR_FP
	stp	q0, q1, [x16], #32
	stp	q2, q3, [x16], #32
	stp	q4, q5, [x16], #32
	stp	q6, q7, [x16], #32
	stp	q8, q9, [x16], #32
	stp	q10, q11, [x16], #32
	stp	q12, q13, [x16], #32
	stp	q14, q15, [x16], #32
	stp	q16, q17, [x16], #32
	stp	q18, q19, [x16], #32
	stp	q20, q21, [x16], #32
	stp	q22, q23, [x16], #32
	stp	q24, q25, [x16], #32
	stp	q26, q27, [x16], #32
	stp	q28, q29, [x16], #32
	stp	q30, q31, [x16]
	mrs	x0, fpcr
	str	x0, [x18, #AVR_FPCR]
	mrs	x0, fpsr
	str	x0, [x18, #AVR_FPSR]

	/* Save the guest keys while HCR_EL2 still selects guest execution. */
	add	x16, x18, #AVR_PAUTH
	mrs	x0, apiakeylo_el1
	mrs	x1, apiakeyhi_el1
	stp	x0, x1, [x16, #0]
	mrs	x0, apibkeylo_el1
	mrs	x1, apibkeyhi_el1
	stp	x0, x1, [x16, #16]
	mrs	x0, apdakeylo_el1
	mrs	x1, apdakeyhi_el1
	stp	x0, x1, [x16, #32]
	mrs	x0, apdbkeylo_el1
	mrs	x1, apdbkeyhi_el1
	stp	x0, x1, [x16, #48]
	mrs	x0, apgakeylo_el1
	mrs	x1, apgakeyhi_el1
	stp	x0, x1, [x16, #64]

	/* Save state shared by both VHE and nVHE guest execution. */
	mrs	x0, sp_el1
	str	x0, [x18, #AVR_SP]
	mrs	x0, sp_el0
	str	x0, [x18, #AVR_SP_EL0]
	mrs	x0, tpidr_el0
	str	x0, [x18, #AVR_TPIDR_EL0]
	mrs	x0, tpidrro_el0
	str	x0, [x18, #AVR_TPIDRRO_EL0]
	mrs	x0, elr_el2
	str	x0, [x18, #AVR_PC]
	mrs	x0, spsr_el2
	str	x0, [x18, #AVR_PSTATE]
	ldr	x1, [x18, #AVR_MODE]
	cmp	x1, #ARM64_VMM_MODE_VHE
	b.eq	.Lsave_guest_vhe

	/* In nVHE the guest occupied the physical EL1 register bank. */
	mrs	x0, elr_el1
	str	x0, [x18, #AVR_ELR_EL1]
	mrs	x0, spsr_el1
	str	x0, [x18, #AVR_SPSR_EL1]
	mrs	x0, esr_el1
	str	x0, [x18, #AVR_ESR_EL1]
	mrs	x0, far_el1
	str	x0, [x18, #AVR_FAR_EL1]
	mrs	x0, sctlr_el1
	str	x0, [x18, #AVR_SCTLR_EL1]
	mrs	x0, tcr_el1
	str	x0, [x18, #AVR_TCR_EL1]
	mrs	x0, ttbr0_el1
	str	x0, [x18, #AVR_TTBR0_EL1]
	mrs	x0, ttbr1_el1
	str	x0, [x18, #AVR_TTBR1_EL1]
	mrs	x0, mair_el1
	str	x0, [x18, #AVR_MAIR_EL1]
	mrs	x0, vbar_el1
	str	x0, [x18, #AVR_VBAR_EL1]
	mrs	x0, contextidr_el1
	str	x0, [x18, #AVR_CONTEXTIDR_EL1]
	mrs	x0, cpacr_el1
	str	x0, [x18, #AVR_CPACR_EL1]
	mrs	x0, cntkctl_el1
	str	x0, [x18, #AVR_CNTKCTL_EL1]
	mrs	x0, tpidr_el1
	str	x0, [x18, #AVR_TPIDR_EL1]
	b	.Lsave_guest_done

.Lsave_guest_vhe:
	/*
	 * A non-final physical-interrupt exit is followed immediately by a
	 * same-CPU retry.  EL12 is a separate hardware bank under VHE and host
	 * interrupt handling cannot modify it, so leave it resident rather than
	 * forcing a long series of nested reads.  Synchronous exits and the final
	 * bounded asynchronous exit take the full snapshot below.
	 */
	ldr	x0, [x18, #AVR_EXIT]
	cmp	x0, #ARM64_VMM_EXIT_IRQ
	b.eq	.Lsave_guest_vhe_async
	cmp	x0, #ARM64_VMM_EXIT_FIQ
	b.ne	.Lsave_guest_vhe_full
.Lsave_guest_vhe_async:
	ldr	x0, [x18, #AVR_HOST_ESR_EL1]
	cbz	x0, .Lsave_guest_done

.Lsave_guest_vhe_full:
	/* In VHE the guest EL1 bank remains accessible through EL12. */
	mrs	x0, elr_el12
	str	x0, [x18, #AVR_ELR_EL1]
	mrs	x0, spsr_el12
	str	x0, [x18, #AVR_SPSR_EL1]
	mrs	x0, esr_el12
	str	x0, [x18, #AVR_ESR_EL1]
	mrs	x0, far_el12
	str	x0, [x18, #AVR_FAR_EL1]
	mrs	x0, sctlr_el12
	str	x0, [x18, #AVR_SCTLR_EL1]
	mrs	x0, tcr_el12
	str	x0, [x18, #AVR_TCR_EL1]
	mrs	x0, ttbr0_el12
	str	x0, [x18, #AVR_TTBR0_EL1]
	mrs	x0, ttbr1_el12
	str	x0, [x18, #AVR_TTBR1_EL1]
	mrs	x0, mair_el12
	str	x0, [x18, #AVR_MAIR_EL1]
	mrs	x0, vbar_el12
	str	x0, [x18, #AVR_VBAR_EL1]
	mrs	x0, contextidr_el12
	str	x0, [x18, #AVR_CONTEXTIDR_EL1]
	mrs	x0, cpacr_el12
	str	x0, [x18, #AVR_CPACR_EL1]
	mrs	x0, cntkctl_el12
	str	x0, [x18, #AVR_CNTKCTL_EL1]
	mrs	x0, tpidr_el1
	str	x0, [x18, #AVR_TPIDR_EL1]

.Lsave_guest_done:
	/*
	 * VMCR retains PMR, BPR, and group enable.  LR0 may have moved from
	 * pending to active or invalid when the guest executed IAR/EOIR, so capture
	 * both before restoring the host virtual CPU interface.
	 */
	mrs	x0, ICH_VMCR_EL2
	str	x0, [x18, #AVR_ICH_VMCR_EL2]
	mrs	x0, ICH_LR0_EL2
	str	x0, [x18, #AVR_ICH_LR0_EL2]
	/* Capture the EL2 syndrome after all guest-visible EL1 state is safe. */
	mrs	x0, esr_el2
	str	x0, [x18, #AVR_ESR_EL2]
	mrs	x0, far_el2
	str	x0, [x18, #AVR_FAR_EL2]
	mrs	x0, hpfar_el2
	str	x0, [x18, #AVR_HPFAR_EL2]
	/*
	 * Restore a deferred VHE host deadline before HCR_EL2 selects the host
	 * context again.  Keep the timer masked while replacing an expired CVAL,
	 * then restore the saved control word.  The ordinary OpenBSD handler runs
	 * after our ERET if the original deadline passed while the guest ran.
	 */
	ldr	x0, [x18, #AVR_MODE]
	cmp	x0, #ARM64_VMM_MODE_VHE
	b.ne	.Lrestore_cnthv_done
	ldr	x0, [x18, #AVR_DEFER_CNTHV]
	cbz	x0, .Lrestore_cnthv_done
	ldr	x0, [x18, #AVR_HOST_CNTHV_CTL_EL2]
	orr	x0, x0, #CNTV_CTL_IMASK
	msr	s3_4_c14_c3_1, x0	/* CNTHV_CTL_EL2 */
	ldr	x0, [x18, #AVR_HOST_CNTHV_CVAL_EL2]
	msr	s3_4_c14_c3_2, x0	/* CNTHV_CVAL_EL2 */
	ldr	x0, [x18, #AVR_HOST_CNTHV_CTL_EL2]
	msr	s3_4_c14_c3_1, x0	/* CNTHV_CTL_EL2 */
	isb
.Lrestore_cnthv_done:

	/*
	 * Restore the host keys before any authenticated host return can execute.
	 * The ISB makes the replacement effective before ERET reaches code built
	 * with PAC return signing.
	 */
	add	x16, x18, #AVR_HOST_PAUTH
	ldp	x0, x1, [x16, #0]
	msr	apiakeylo_el1, x0
	msr	apiakeyhi_el1, x1
	ldp	x0, x1, [x16, #16]
	msr	apibkeylo_el1, x0
	msr	apibkeyhi_el1, x1
	ldp	x0, x1, [x16, #32]
	msr	apdakeylo_el1, x0
	msr	apdakeyhi_el1, x1
	ldp	x0, x1, [x16, #48]
	msr	apdbkeylo_el1, x0
	msr	apdbkeyhi_el1, x1
	ldp	x0, x1, [x16, #64]
	msr	apgakeylo_el1, x0
	msr	apgakeyhi_el1, x1
	isb

	ldr	x1, [x18, #AVR_MODE]
	cmp	x1, #ARM64_VMM_MODE_VHE
	b.eq	.Lrestore_vhe

	/*
	 * Disable stage 2 before restoring the nVHE host EL1 translation
	 * regime.  No exit-side TLBI is needed: a later mapping change marks
	 * AVR_FLUSH_TLB, and otherwise the same VMID/context remains valid.
	 */
	/* Disable the guest interface, then put back the host's complete view. */
	msr	ICH_HCR_EL2, xzr
	ldr	x0, [x18, #AVR_HOST_ICH_VMCR_EL2]
	msr	ICH_VMCR_EL2, x0
	ldr	x0, [x18, #AVR_HOST_ICH_LR0_EL2]
	msr	ICH_LR0_EL2, x0
	ldr	x0, [x18, #AVR_HOST_ICH_HCR_EL2]
	msr	ICH_HCR_EL2, x0
	ldr	x0, [x18, #AVR_HOST_HCR_EL2]
	msr	hcr_el2, x0
	isb

	ldr	x0, [x18, #AVR_HOST_TCR_EL1]
	msr	tcr_el1, x0
	ldr	x0, [x18, #AVR_HOST_TTBR0_EL1]
	msr	ttbr0_el1, x0
	ldr	x0, [x18, #AVR_HOST_TTBR1_EL1]
	msr	ttbr1_el1, x0
	ldr	x0, [x18, #AVR_HOST_MAIR_EL1]
	msr	mair_el1, x0
	ldr	x0, [x18, #AVR_HOST_VBAR_EL1]
	msr	vbar_el1, x0
	ldr	x0, [x18, #AVR_HOST_CONTEXTIDR_EL1]
	msr	contextidr_el1, x0
	ldr	x0, [x18, #AVR_HOST_CPACR_EL1]
	msr	cpacr_el1, x0
	ldr	x0, [x18, #AVR_HOST_CNTKCTL_EL1]
	msr	cntkctl_el1, x0
	ldr	x0, [x18, #AVR_HOST_ELR_EL1]
	msr	elr_el1, x0
	ldr	x0, [x18, #AVR_HOST_SPSR_EL1]
	msr	spsr_el1, x0
	ldr	x0, [x18, #AVR_HOST_ESR_EL1]
	msr	esr_el1, x0
	ldr	x0, [x18, #AVR_HOST_FAR_EL1]
	msr	far_el1, x0
	ldr	x0, [x18, #AVR_HOST_SP_EL0]
	msr	sp_el0, x0
	ldr	x0, [x18, #AVR_HOST_TPIDR_EL0]
	msr	tpidr_el0, x0
	ldr	x0, [x18, #AVR_HOST_TPIDRRO_EL0]
	msr	tpidrro_el0, x0
	ldr	x0, [x18, #AVR_HOST_TPIDR_EL1]
	msr	tpidr_el1, x0
	ldr	x0, [x18, #AVR_HOST_CNTHCTL_EL2]
	msr	cnthctl_el2, x0
	ldr	x0, [x18, #AVR_HOST_VMPIDR_EL2]
	msr	vmpidr_el2, x0
	ldr	x0, [x18, #AVR_HOST_SCTLR_EL1]
	msr	sctlr_el1, x0
	isb

	/* ERET resumes the instruction after HVC in arm64_vmm_enter_nvhe(). */
	ldr	x0, [x18, #AVR_HOST_SP]
	msr	sp_el1, x0
	ldr	x0, [x18, #AVR_HOST_PC]
	msr	elr_el2, x0
	ldr	x0, [x18, #AVR_HOST_PSTATE]
	msr	spsr_el2, x0
	msr	tpidr_el2, xzr
	mov	x0, #0
	eret

.Lrestore_vhe:
	/*
	 * Re-enable TGE for the VHE host; its EL2 stage-1 translation regime
	 * stayed live throughout the guest run.  The guest used EL12, so only
	 * genuinely shared EL0/EL1 registers need explicit restoration here.
	 */
	/* Restore the VHE host's complete virtual CPU-interface context. */
	msr	ICH_HCR_EL2, xzr
	ldr	x0, [x18, #AVR_HOST_ICH_VMCR_EL2]
	msr	ICH_VMCR_EL2, x0
	ldr	x0, [x18, #AVR_HOST_ICH_LR0_EL2]
	msr	ICH_LR0_EL2, x0
	ldr	x0, [x18, #AVR_HOST_ICH_HCR_EL2]
	msr	ICH_HCR_EL2, x0
	ldr	x0, [x18, #AVR_HOST_HCR_EL2]
	msr	hcr_el2, x0
	isb
	ldr	x0, [x18, #AVR_HOST_VBAR_EL1]
	msr	vbar_el2, x0
	ldr	x0, [x18, #AVR_HOST_CONTEXTIDR_EL1]
	msr	contextidr_el1, x0
	ldr	x0, [x18, #AVR_HOST_CPACR_EL1]
	msr	cpacr_el1, x0
	ldr	x0, [x18, #AVR_HOST_CNTKCTL_EL1]
	msr	cntkctl_el1, x0
	ldr	x0, [x18, #AVR_HOST_SP_EL0]
	msr	sp_el0, x0
	ldr	x0, [x18, #AVR_HOST_TPIDR_EL0]
	msr	tpidr_el0, x0
	ldr	x0, [x18, #AVR_HOST_TPIDRRO_EL0]
	msr	tpidrro_el0, x0
	ldr	x0, [x18, #AVR_HOST_TPIDR_EL1]
	msr	tpidr_el1, x0
	ldr	x0, [x18, #AVR_HOST_CNTHCTL_EL2]
	msr	cnthctl_el2, x0
	ldr	x0, [x18, #AVR_HOST_VMPIDR_EL2]
	msr	vmpidr_el2, x0
	/* ERET lands on .Lvhe_return with the original DAIF and NZCV restored. */
	ldr	x0, [x18, #AVR_HOST_PC]
	msr	elr_el2, x0
	ldr	x0, [x18, #AVR_HOST_PSTATE]
	msr	spsr_el2, x0
	msr	tpidr_el2, xzr
	mov	x0, #0
	eret

.Lbad_hyp:
	b	.Lbad_hyp
END(vmm_hyp_sync)
