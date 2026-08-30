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

	/* x0 = PrimeCell UART0; x1 walks the NUL-terminated message. */
	ldr	x0, =0x09000000
	adr	x1, message

1:
	ldrb	w2, [x1], #1
	cbz	w2, 3f

	/* Bit 5 of PL011_FR is TXFF: wait while the transmit FIFO is full. */
2:
	ldr	w3, [x0, #0x18]
	tbnz	w3, #5, 2b
	str	w2, [x0]		/* PL011_DR, one 32-bit MMIO write. */
	b	1b

3:
	/* vmd treats HVC as a halted vCPU until the VM is stopped. */
	hvc	#0
	b	3b

	.p2align 3
message:
	.asciz	"arm64 vmd PL011 works\r\n"
	.size _start, . - _start
