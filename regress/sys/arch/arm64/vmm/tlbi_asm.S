/*	$OpenBSD$	*/
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

	/* These bytes are copied into guest RAM, so keep them readable to C. */
	.section .rodata
	.p2align 2

	.global tlbi_boot_start
	.global tlbi_boot_end
tlbi_boot_start:
	eret
tlbi_boot_end:

	.global tlbi_fault_start
	.global tlbi_fault_end
tlbi_fault_start:
	/* Model the paired kernel/user ASIDs used by the OpenBSD trampoline. */
	msr	ttbr1_el1, x8
	isb
	/*
	 * Count retries and install the missing table or leaf.  x5 selects one of
	 * the EL1 invalidation families exercised by tlbi.c.  Each case starts
	 * in a new VM so no invalidation performed by an earlier case can make
	 * a later one pass accidentally.
	 */
	add	x4, x4, #1
	str	x1, [x0]
	dsb	ishst
	cmp	x5, #0
	b.eq	1f
	cmp	x5, #1
	b.eq	2f
	cmp	x5, #2
	b.eq	3f
	tlbi	vmalle1is
	b	4f
1:
	tlbi	vae1is, x2
	b	4f
2:
	tlbi	vaale1is, x2
	b	4f
3:
	/* ASIDE1IS takes only the ASID in bits [63:48]. */
	lsr	x6, x2, #48
	lsl	x6, x6, #48
	tlbi	aside1is, x6
4:
	dsb	ish
	isb
	/* A later SVC must use the success vector instead of mapping again. */
	msr	vbar_el1, x3
	msr	ttbr1_el1, x7
	isb
	eret
tlbi_fault_end:

	.global tlbi_target_start
	.global tlbi_target_end
tlbi_target_start:
	mov	x0, #0x42
	svc	#0
tlbi_target_end:

	.global tlbi_success_start
	.global tlbi_success_end
tlbi_success_start:
	hvc	#0
tlbi_success_end:
