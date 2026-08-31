/*	$OpenBSD$	*/

	.text
	.p2align 2
	.global _start
	.type _start, %function
_start:
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
