/*	$OpenBSD */

/*
 * Copyright (c) 2025 Mike Larkin <mlarkin@openbsd.org>
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

#include <machine/i82489reg.h>

#include "i82489dx.h"
#include "mmio.h"
#include "vmd.h"

struct i82489dx {
	uint64_t	base;
	uint32_t	ver;
};

/* XXX will need one per cpu, which entails a per-vcpu init loop in x86_vm.c
 *     for cases like this. The vcpu in use will also need to be passed
 *     through to all io functions.
 */
struct i82489dx		lapic;

uint32_t i82489_get_version(void);

uint32_t
i82489_get_version(void)
{
	log_warnx("%s: returning 0x%x", __func__, lapic.ver);

	return lapic.ver;
}

int
i82489dx_mmio(int dir, paddr_t addr, uint64_t *data)
{
	uint16_t reg;

	log_warnx("%s: dir=%d addr=0x%lx data=0x%llx", __func__, dir, addr,
	    *data);

	reg = addr - lapic.base;
	if (reg > 0xFFF) {
		log_warnx("%s: invalid i82489 register 0x%x", __func__, reg);
		return 1;
	}

	switch (reg) {
	case LAPIC_VERS:
		if (dir == MMIO_DIR_READ) {
			*data &= 0xFFFFFFFF00000000;
			*data |= i82489_get_version();
			log_warnx("%s: read LAPIC_VERS: return 0x%llx",
			    __func__, *data);
		} else {
			log_warnx("%s: write to reg 0x%x discarded", __func__,
			    reg);
		}
		break;
	default:
		if (dir == MMIO_DIR_READ) {
			log_warnx("%s: unsupported i/o on i82489 reg 0x%x. "
			    "returning 0xFFFFFFFFFFFFFFFF", __func__, reg);
			*data = 0xFFFFFFFFFFFFFFFF;
		} else {
			log_warnx("%s: discarding write to reg 0x%x", __func__,
			    reg);
		}
	}

	return 0;
}

void
i82489dx_init(void)
{
	lapic.ver = (1ULL << 31) | (6ULL << LAPIC_VERSION_LVT_SHIFT) | 0x10;
	lapic.base = LAPIC_BASE;

	mmio_dev_add(LAPIC_BASE, LAPIC_BASE + 0xFFF,
	    (mmio_dev_fn_t)i82489dx_mmio);
}
