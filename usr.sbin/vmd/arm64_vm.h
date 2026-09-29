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

#ifndef _VMD_ARM64_VM_H_
#define _VMD_ARM64_VM_H_

/*
 * Initial "virt"-style physical map.  RAM and the PL011 addresses match the
 * conventional QEMU arm64 layout, which gives a later firmware/FDT layer a
 * familiar ABI without requiring one for the first payload test.
 *
 * The current vmm(4) arm64 stage-2 tables use a 16KB granule.  OpenBSD itself
 * uses 4KB PAGE_SIZE pages here, hence the four-page expression.  MMIO ranges
 * and RAM sizes presented to vmm(4) must respect the stage-2 granule.
 */
#define ARM64_STAGE2_PAGE_SIZE	(4 * PAGE_SIZE)
#define ARM64_GICD_BASE		0x08000000UL
#define ARM64_GICD_SIZE		0x00010000UL
#define ARM64_GICR_BASE		0x080a0000UL
#define ARM64_GICR_SIZE		0x00020000UL
#define ARM64_RAM_BASE		0x40000000UL
#define ARM64_UART_BASE		0x09000000UL
#define ARM64_UART_SIZE		ARM64_STAGE2_PAGE_SIZE
#define ARM64_UART_INTID	33
#define ARM64_FDT_SIZE		ARM64_STAGE2_PAGE_SIZE

int	arm64_vcpu_intr(int, uint32_t, uint16_t, uint8_t, int);
int	arm64_vcpu_irqcfg(int, uint32_t, uint16_t, uint8_t, int);
int	arm64_fdt_build(void *, size_t, size_t, size_t *);

#endif /* _VMD_ARM64_VM_H_ */
