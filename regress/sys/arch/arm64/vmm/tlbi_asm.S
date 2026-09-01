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
	/* Count retries, install the missing L3 entry, then invalidate its VA. */
	add	x4, x4, #1
	str	x1, [x0]
	dsb	ishst
	tlbi	vae1is, x2
	dsb	ish
	isb
	/* A later SVC must use the success vector instead of mapping again. */
	msr	vbar_el1, x3
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
