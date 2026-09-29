/*	$OpenBSD$ */
/* See gicstate.c for the license and test sequence. */

	/* Copied as data into guest RAM, not executed by the host process. */
	.section .rodata
	.balign 16
	.globl gicstate_start, gicstate_end
gicstate_start:
	mov	x0, #0xff
	msr	ICC_PMR_EL1, x0
	mov	x0, #1
	msr	ICC_IGRPEN1_EL1, x0
	isb
	mrs	x2, ICC_RPR_EL1
	hvc	#0
	mrs	x1, ICC_IAR1_EL1
	isb
	mrs	x2, ICC_RPR_EL1
	hvc	#0
	mrs	x2, ICC_RPR_EL1
	hvc	#0
	msr	ICC_EOIR1_EL1, x1
	isb
	mrs	x2, ICC_RPR_EL1
	hvc	#0
gicstate_end:
