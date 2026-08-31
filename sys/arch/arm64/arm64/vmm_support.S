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
 * kernel virtual address in this mode.
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
	mrs	x2, daif
	msr	daifset, #0xf
	mov	x17, x0
	msr	tpidr_el2, x17
	mov	x0, #ARM64_VMM_MODE_VHE
	str	x0, [x17, #AVR_MODE]

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
	mrs	x0, cntvoff_el2
	str	x0, [x17, #AVR_HOST_CNTVOFF_EL2]
	mrs	x0, cnthctl_el2
	str	x0, [x17, #AVR_HOST_CNTHCTL_EL2]
	mrs	x0, vmpidr_el2
	str	x0, [x17, #AVR_HOST_VMPIDR_EL2]
	mrs	x0, hcr_el2
	str	x0, [x17, #AVR_HOST_HCR_EL2]
	/* ICH_HCR_EL2 is per-CPU EL2 state, not part of the guest EL1 bank. */
	mrs	x0, ICH_HCR_EL2
	str	x0, [x17, #AVR_HOST_ICH_HCR_EL2]

	/* All lower-EL AArch64 exceptions now enter the vmm vector slots. */
	adrp	x0, hyp_vectors
	add	x0, x0, :lo12:hyp_vectors
	msr	vbar_el2, x0
	ldr	x0, [x17, #AVR_VTTBR_EL2]
	msr	vttbr_el2, x0
	ldr	x0, [x17, #AVR_VTCR_EL2]
	msr	vtcr_el2, x0
	/* Install ICC trap policy before HCR_EL2.IMO exposes the virtual view. */
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
	ldr	x0, [x17, #AVR_FLUSH_TLB]
	cbz	x0, .Lvhe_tlb_done
	/*
	 * Invalidate only when C marked the stage-2 context dirty.  A global
	 * TLBI on every host interrupt is prohibitively slow under nesting.
	 */
	dsb	ishst
	tlbi	vmalls12e1is
	dsb	ish
	isb
.Lvhe_tlb_done:

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
	stp	x16, x17, [sp, #-16]!
	mrs	x17, tpidr_el2
	cbnz	x17, .Lguest_sync
	ldp	x16, x17, [sp], #16

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

	/* Install the guest's stage-2 root, VMID, and address-space controls. */
	ldr	x0, [x17, #AVR_VTTBR_EL2]
	msr	vttbr_el2, x0
	ldr	x0, [x17, #AVR_VTCR_EL2]
	msr	vtcr_el2, x0
	/* The following HCR write redirects guest ICC accesses to this policy. */
	ldr	x0, [x17, #AVR_ICH_HCR_EL2]
	msr	ICH_HCR_EL2, x0
	ldr	x0, [x17, #AVR_FLUSH_TLB]
	cbz	x0, .Lnvhe_tlb_done
	/* See the VHE path above: C requests this only after mapping changes. */
	dsb	ishst
	tlbi	vmalls12e1is
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
	stp	x16, x17, [sp, #-16]!
	mrs	x17, tpidr_el2
	cbz	x17, .Lbad_hyp
	mov	x16, #ARM64_VMM_EXIT_IRQ
	str	x16, [x17, #AVR_EXIT]
	b	.Lguest_exit
END(vmm_hyp_irq)

ENTRY_NP(vmm_hyp_fiq)
	/* FIQ follows the same transparent host-delivery path as IRQ. */
	stp	x16, x17, [sp, #-16]!
	mrs	x17, tpidr_el2
	cbz	x17, .Lbad_hyp
	mov	x16, #ARM64_VMM_EXIT_FIQ
	str	x16, [x17, #AVR_EXIT]
	b	.Lguest_exit
END(vmm_hyp_fiq)

ENTRY_NP(vmm_hyp_serror)
	/* C currently treats a physical SError observed in a vCPU as fatal. */
	stp	x16, x17, [sp, #-16]!
	mrs	x17, tpidr_el2
	cbz	x17, .Lbad_hyp
	mov	x16, #ARM64_VMM_EXIT_SERROR
	str	x16, [x17, #AVR_EXIT]
END(vmm_hyp_serror)

.Lguest_exit:
	/*
	 * x16/x17 were saved by the vector stub, but all other guest GPRs are
	 * still live.  CNTVOFF_EL2 temporarily holds x18 while x18 becomes the
	 * run-page base; its real value is already present in the run page and
	 * will be restored later.
	 */
	msr	cntvoff_el2, x18
	mrs	x18, tpidr_el2
	ldp	x16, x17, [sp], #16

	stp	x0, x1, [x18, #AVR_GPRS + 0]
	stp	x2, x3, [x18, #AVR_GPRS + 16]
	stp	x4, x5, [x18, #AVR_GPRS + 32]
	stp	x6, x7, [x18, #AVR_GPRS + 48]
	stp	x8, x9, [x18, #AVR_GPRS + 64]
	stp	x10, x11, [x18, #AVR_GPRS + 80]
	stp	x12, x13, [x18, #AVR_GPRS + 96]
	stp	x14, x15, [x18, #AVR_GPRS + 112]
	stp	x16, x17, [x18, #AVR_GPRS + 128]
	stp	x19, x20, [x18, #AVR_GPRS + 152]
	stp	x21, x22, [x18, #AVR_GPRS + 168]
	stp	x23, x24, [x18, #AVR_GPRS + 184]
	stp	x25, x26, [x18, #AVR_GPRS + 200]
	stp	x27, x28, [x18, #AVR_GPRS + 216]
	stp	x29, x30, [x18, #AVR_GPRS + 232]
	mrs	x0, cntvoff_el2
	str	x0, [x18, #AVR_GPRS + 144]

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
	/* Capture the EL2 syndrome after all guest-visible EL1 state is safe. */
	mrs	x0, esr_el2
	str	x0, [x18, #AVR_ESR_EL2]
	mrs	x0, far_el2
	str	x0, [x18, #AVR_FAR_EL2]
	mrs	x0, hpfar_el2
	str	x0, [x18, #AVR_HPFAR_EL2]

	ldr	x1, [x18, #AVR_MODE]
	cmp	x1, #ARM64_VMM_MODE_VHE
	b.eq	.Lrestore_vhe

	/*
	 * Disable stage 2 before restoring the nVHE host EL1 translation
	 * regime.  No exit-side TLBI is needed: a later mapping change marks
	 * AVR_FLUSH_TLB, and otherwise the same VMID/context remains valid.
	 */
	/* Stop trapping ICC accesses before returning to the nVHE host. */
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
	ldr	x0, [x18, #AVR_HOST_CNTVOFF_EL2]
	msr	cntvoff_el2, x0
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
	/* Restore the VHE host's ICH policy just as its HCR policy is restored. */
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
	ldr	x0, [x18, #AVR_HOST_CNTVOFF_EL2]
	msr	cntvoff_el2, x0
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
