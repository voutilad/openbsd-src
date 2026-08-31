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

	.text
	.arch armv8-a+fp
	.align 2
	.globl run_with_fp
	.type run_with_fp,@function
/*
 * int run_with_fp(int fd, unsigned long request, void *arg,
 *     uint64_t pattern, uint64_t *observed)
 *
 * Keep a value in the ABI-preserved d8 register across ioctl(2).  A correct
 * VMM_IOC_RUN saves the calling process's live FP state before loading the
 * guest vector file, then restores it through OpenBSD's lazy-FPU path.
 */
run_with_fp:
	stp	x29, x30, [sp, #-48]!
	mov	x29, sp
	str	x19, [sp, #16]
	str	d8, [sp, #24]
	mov	x19, x4
	fmov	d8, x3
	bl	ioctl
	fmov	x1, d8
	str	x1, [x19]
	ldr	d8, [sp, #24]
	ldr	x19, [sp, #16]
	ldp	x29, x30, [sp], #48
	ret
	.size run_with_fp, .-run_with_fp
