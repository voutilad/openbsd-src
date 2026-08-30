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

#define VMM_HVC_ENTER	0x4f42

/*
 * Enter EL2 with x0 holding the physical address of struct arm64_vmm_run.
 * All callee-saved registers are kept on the host EL1 stack; EL2 restores
 * that stack before returning here.
 */
ENTRY(arm64_vmm_enter_nvhe)
	sub	sp, sp, #96
	stp	x19, x20, [sp, #0]
	stp	x21, x22, [sp, #16]
	stp	x23, x24, [sp, #32]
	stp	x25, x26, [sp, #48]
	stp	x27, x28, [sp, #64]
	stp	x29, x30, [sp, #80]

	hvc	#VMM_HVC_ENTER

	ldp	x19, x20, [sp, #0]
	ldp	x21, x22, [sp, #16]
	ldp	x23, x24, [sp, #32]
	ldp	x25, x26, [sp, #48]
	ldp	x27, x28, [sp, #64]
	ldp	x29, x30, [sp, #80]
	add	sp, sp, #96
	ret
END(arm64_vmm_enter_nvhe)

/*
 * Enter a guest from a VHE host.  HCR_EL2.E2H remains set, so the host's EL2
 * stage-1 translation regime stays active and the run-page pointer is a
 * kernel virtual address in this mode.
 */
ENTRY(arm64_vmm_enter_vhe)
	sub	sp, sp, #96
	stp	x19, x20, [sp, #0]
	stp	x21, x22, [sp, #16]
	stp	x23, x24, [sp, #32]
	stp	x25, x26, [sp, #48]
	stp	x27, x28, [sp, #64]
	stp	x29, x30, [sp, #80]

	/* The temporary EL2 vector table handles synchronous guest exits only. */
	mrs	x2, daif
	msr	daifset, #0xf
	mov	x17, x0
	msr	tpidr_el2, x17
	mov	x0, #ARM64_VMM_MODE_VHE
	str	x0, [x17, #AVR_MODE]

	mov	x0, sp
	str	x0, [x17, #AVR_HOST_SP]
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
	mrs	x0, tpidr_el1
	str	x0, [x17, #AVR_HOST_TPIDR_EL1]
	mrs	x0, cntvoff_el2
	str	x0, [x17, #AVR_HOST_CNTVOFF_EL2]
	mrs	x0, hcr_el2
	str	x0, [x17, #AVR_HOST_HCR_EL2]

	adrp	x0, hyp_vectors
	add	x0, x0, :lo12:hyp_vectors
	msr	vbar_el2, x0
	ldr	x0, [x17, #AVR_VTTBR_EL2]
	msr	vttbr_el2, x0
	ldr	x0, [x17, #AVR_VTCR_EL2]
	msr	vtcr_el2, x0

	/* Clear TGE but retain E2H and access guest state via the EL12 aliases. */
	ldr	x0, [x17, #AVR_HCR_EL2]
	msr	hcr_el2, x0
	isb
	dsb	ishst
	tlbi	vmalls12e1
	tlbi	vmalls12e1is
	dsb	ish
	isb

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
	ldr	x0, [x17, #AVR_TPIDR_EL1]
	msr	tpidr_el1, x0
	ldr	x0, [x17, #AVR_CNTVOFF_EL2]
	msr	cntvoff_el2, x0
	ldr	x0, [x17, #AVR_SP]
	msr	sp_el1, x0
	ldr	x0, [x17, #AVR_PC]
	msr	elr_el2, x0
	ldr	x0, [x17, #AVR_PSTATE]
	msr	spsr_el2, x0
	dsb	ish
	ic	ialluis
	dsb	ish
	isb

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
	ldp	x19, x20, [sp, #0]
	ldp	x21, x22, [sp, #16]
	ldp	x23, x24, [sp, #32]
	ldp	x25, x26, [sp, #48]
	ldp	x27, x28, [sp, #64]
	ldp	x29, x30, [sp, #80]
	add	sp, sp, #96
	ret
END(arm64_vmm_enter_vhe)

/*
 * EL2 synchronous vector.  tpidr_el2 is zero for a host entry and contains
 * the physical run-page address while a guest is active.
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

	/* x0 is safe to dereference here: EL2 uses physical addressing. */
	mov	x17, x0
	msr	tpidr_el2, x17
	mov	x0, #ARM64_VMM_MODE_NVHE
	str	x0, [x17, #AVR_MODE]

	/* Save the host state shared with EL1. */
	mrs	x0, sp_el1
	str	x0, [x17, #AVR_HOST_SP]
	mrs	x0, elr_el2
	str	x0, [x17, #AVR_HOST_PC]
	mrs	x0, spsr_el2
	str	x0, [x17, #AVR_HOST_PSTATE]
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
	mrs	x0, tpidr_el1
	str	x0, [x17, #AVR_HOST_TPIDR_EL1]
	mrs	x0, cntvoff_el2
	str	x0, [x17, #AVR_HOST_CNTVOFF_EL2]
	mrs	x0, hcr_el2
	str	x0, [x17, #AVR_HOST_HCR_EL2]

	/* Install the guest's stage-2 context. */
	ldr	x0, [x17, #AVR_VTTBR_EL2]
	msr	vttbr_el2, x0
	ldr	x0, [x17, #AVR_VTCR_EL2]
	msr	vtcr_el2, x0
	dsb	ishst
	tlbi	vmalls12e1
	tlbi	vmalls12e1is
	dsb	ish
	isb

	/* Disable guest stage 1 before replacing its translation registers. */
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
	ldr	x0, [x17, #AVR_TPIDR_EL1]
	msr	tpidr_el1, x0
	ldr	x0, [x17, #AVR_CNTVOFF_EL2]
	msr	cntvoff_el2, x0

	ldr	x0, [x17, #AVR_SP]
	msr	sp_el1, x0
	ldr	x0, [x17, #AVR_PC]
	msr	elr_el2, x0
	ldr	x0, [x17, #AVR_PSTATE]
	msr	spsr_el2, x0
	ldr	x0, [x17, #AVR_HCR_EL2]
	msr	hcr_el2, x0
	isb
	dsb	ish
	ic	ialluis
	dsb	ish
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
	mov	x16, #ARM64_VMM_EXIT_SYNC
	str	x16, [x17, #AVR_EXIT]
	b	.Lguest_exit

ENTRY_NP(vmm_hyp_irq)
	stp	x16, x17, [sp, #-16]!
	mrs	x17, tpidr_el2
	cbz	x17, .Lbad_hyp
	mov	x16, #ARM64_VMM_EXIT_IRQ
	str	x16, [x17, #AVR_EXIT]
	b	.Lguest_exit
END(vmm_hyp_irq)

ENTRY_NP(vmm_hyp_fiq)
	stp	x16, x17, [sp, #-16]!
	mrs	x17, tpidr_el2
	cbz	x17, .Lbad_hyp
	mov	x16, #ARM64_VMM_EXIT_FIQ
	str	x16, [x17, #AVR_EXIT]
	b	.Lguest_exit
END(vmm_hyp_fiq)

ENTRY_NP(vmm_hyp_serror)
	stp	x16, x17, [sp, #-16]!
	mrs	x17, tpidr_el2
	cbz	x17, .Lbad_hyp
	mov	x16, #ARM64_VMM_EXIT_SERROR
	str	x16, [x17, #AVR_EXIT]
END(vmm_hyp_serror)

.Lguest_exit:
	/* Preserve guest x18 while obtaining the physical run-page pointer. */
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

	mrs	x0, sp_el1
	str	x0, [x18, #AVR_SP]
	mrs	x0, elr_el2
	str	x0, [x18, #AVR_PC]
	mrs	x0, spsr_el2
	str	x0, [x18, #AVR_PSTATE]
	ldr	x1, [x18, #AVR_MODE]
	cmp	x1, #ARM64_VMM_MODE_VHE
	b.eq	.Lsave_guest_vhe

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
	mrs	x0, tpidr_el1
	str	x0, [x18, #AVR_TPIDR_EL1]
	b	.Lsave_guest_done

.Lsave_guest_vhe:
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
	mrs	x0, tpidr_el1
	str	x0, [x18, #AVR_TPIDR_EL1]

.Lsave_guest_done:
	mrs	x0, esr_el2
	str	x0, [x18, #AVR_ESR_EL2]
	mrs	x0, far_el2
	str	x0, [x18, #AVR_FAR_EL2]
	mrs	x0, hpfar_el2
	str	x0, [x18, #AVR_HPFAR_EL2]

	ldr	x1, [x18, #AVR_MODE]
	cmp	x1, #ARM64_VMM_MODE_VHE
	b.eq	.Lrestore_vhe

	/* Disable stage 2 before restoring the host EL1 translation regime. */
	ldr	x0, [x18, #AVR_HOST_HCR_EL2]
	msr	hcr_el2, x0
	isb
	dsb	ishst
	tlbi	vmalls12e1
	tlbi	vmalls12e1is
	dsb	ish
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
	ldr	x0, [x18, #AVR_HOST_TPIDR_EL1]
	msr	tpidr_el1, x0
	ldr	x0, [x18, #AVR_HOST_CNTVOFF_EL2]
	msr	cntvoff_el2, x0
	ldr	x0, [x18, #AVR_HOST_SCTLR_EL1]
	msr	sctlr_el1, x0
	isb

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
	/* Re-enable the VHE host regime; the EL2 translation state stayed live. */
	dsb	ishst
	tlbi	vmalls12e1
	tlbi	vmalls12e1is
	dsb	ish
	isb
	ldr	x0, [x18, #AVR_HOST_HCR_EL2]
	msr	hcr_el2, x0
	isb
	ldr	x0, [x18, #AVR_HOST_VBAR_EL1]
	msr	vbar_el2, x0
	ldr	x0, [x18, #AVR_HOST_CONTEXTIDR_EL1]
	msr	contextidr_el1, x0
	ldr	x0, [x18, #AVR_HOST_CPACR_EL1]
	msr	cpacr_el1, x0
	ldr	x0, [x18, #AVR_HOST_TPIDR_EL1]
	msr	tpidr_el1, x0
	ldr	x0, [x18, #AVR_HOST_CNTVOFF_EL2]
	msr	cntvoff_el2, x0
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
