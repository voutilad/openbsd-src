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

#include <sys/param.h>
#include <sys/types.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "arm64_vm.h"
#include "pl011.h"

#define PL011_DR	0x00
#define PL011_FR	0x18
#define  PL011_FR_RXFE	(1U << 4)
#define  PL011_FR_TXFE	(1U << 7)
#define PL011_REG_LAST	0x48

struct pl011_dev {
	int		pd_fd;
	uint32_t	pd_regs[(PL011_REG_LAST / sizeof(uint32_t)) + 1];
};

static struct pl011_dev pl011;

void
pl011_init(int fd)
{
	memset(&pl011, 0, sizeof(pl011));
	pl011.pd_fd = fd;
}

int
pl011_mmio(paddr_t gpa, int iswrite, uint32_t *data)
{
	paddr_t offset;
	uint8_t ch;
	ssize_t n;

	if (gpa < ARM64_UART_BASE ||
	    gpa - ARM64_UART_BASE >= ARM64_UART_SIZE)
		return (EFAULT);
	offset = gpa - ARM64_UART_BASE;
	if ((offset & (sizeof(uint32_t) - 1)) != 0)
		return (EINVAL);

	if (offset == PL011_DR) {
		if (!iswrite) {
			*data = 0;
			return (0);
		}
		ch = *data;
		do {
			n = write(pl011.pd_fd, &ch, sizeof(ch));
		} while (n == -1 && errno == EINTR);
		/*
		 * The PTY slave is closed whenever no vmctl console is attached.
		 * Just as with ns8250, serial output is best-effort: a host-side
		 * write failure must not turn a disconnected console into a fatal
		 * vCPU exit.  FR continues to advertise an empty transmit FIFO.
		 */
		return (0);
	}

	if (offset == PL011_FR) {
		if (iswrite)
			return (0);
		*data = PL011_FR_RXFE | PL011_FR_TXFE;
		return (0);
	}

	if (offset > PL011_REG_LAST)
		return (EFAULT);
	if (iswrite)
		pl011.pd_regs[offset / sizeof(uint32_t)] = *data;
	else
		*data = pl011.pd_regs[offset / sizeof(uint32_t)];
	return (0);
}
