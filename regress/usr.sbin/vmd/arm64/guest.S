/*	$OpenBSD$	*/

	.text
	.p2align 2
	.global _start
	.type _start, %function
_start:
	/* Give "vmctl start -c" time to attach the PTY slave. */
	ldr	x4, =100000000
0:
	subs	x4, x4, #1
	b.ne	0b

	/* PrimeCell UART0, at the fixed address in arm64_vm.h. */
	ldr	x0, =0x09000000
	adr	x1, message

1:
	ldrb	w2, [x1], #1
	cbz	w2, 3f

	/* Wait while PL011_FR says its transmit FIFO is full. */
2:
	ldr	w3, [x0, #0x18]
	tbnz	w3, #5, 2b
	str	w2, [x0]
	b	1b

3:
	/* vmd treats HVC as a halted vCPU until the VM is stopped. */
	hvc	#0
	b	3b

	.p2align 3
message:
	.asciz	"arm64 vmd PL011 works\r\n"
	.size _start, . - _start
