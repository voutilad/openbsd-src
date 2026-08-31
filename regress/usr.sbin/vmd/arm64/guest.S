/*	$OpenBSD$	*/

	.text
	.p2align 2
	.global _start
	.type _start, %function
_start:
	/* Exception vectors must be live before guest_main unmasks IRQs. */
	adr	x0, guest_vectors
	msr	vbar_el1, x0
	isb

	/*
	 * vmd creates the PTY while starting the VM.  Give uart.sh time to
	 * open its slave before producing the one message the test verifies.
	 * This loop is bounded; after output, HVC provides the real idle state.
	 */
	ldr	x4, =100000000
0:
	subs	x4, x4, #1
	b.ne	0b

	/* OpenBSD's arm64 boot ABI supplies the FDT physical address in x2. */
	mov	x0, x2
	bl	guest_main

1:
	/* vmd treats HVC as a halted vCPU until the VM is stopped. */
	hvc	#0
	b	1b
	.size _start, . - _start

	/* VBAR_EL1 requires 2KB alignment and each vector owns a 128-byte slot. */
	.balign 2048
	.global guest_vectors
guest_vectors:
	/* Current EL using SP_EL1 selects the IRQ slot at offset 0x280. */
	.space	0x280
	b	guest_irq_entry
	.space	0x800 - 0x284

	/* Preserve the interrupted C context, call the test handler, and return. */
guest_irq_entry:
	sub	sp, sp, #256
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30, [sp, #240]
	bl	guest_irq
	ldr	x30, [sp, #240]
	ldp	x28, x29, [sp, #224]
	ldp	x26, x27, [sp, #208]
	ldp	x24, x25, [sp, #192]
	ldp	x22, x23, [sp, #176]
	ldp	x20, x21, [sp, #160]
	ldp	x18, x19, [sp, #144]
	ldp	x16, x17, [sp, #128]
	ldp	x14, x15, [sp, #112]
	ldp	x12, x13, [sp, #96]
	ldp	x10, x11, [sp, #80]
	ldp	x8, x9, [sp, #64]
	ldp	x6, x7, [sp, #48]
	ldp	x4, x5, [sp, #32]
	ldp	x2, x3, [sp, #16]
	ldp	x0, x1, [sp, #0]
	add	sp, sp, #256
	eret
