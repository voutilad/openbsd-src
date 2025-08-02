/*	$OpenBSD */

/*
 * Copyright (c) 2024 Mike Larkin <mlarkin@openbsd.org>
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

#include <machine/i82093reg.h>

#include "i82093aa.h"
#include "mmio.h"
#include "vmd.h"
#include "x86_mmio.h"

struct i82093aa {
	uint32_t	reg;
	uint32_t	win;

	uint32_t	id;
};

struct i82093aa		ioapic;

static void
i82093aa_winop(int dir, uint64_t *data)
{
	uint32_t d;

	if (dir == MMIO_DIR_READ) {
		log_warnx("%s: requested read from register 0x%x", __func__,
		    ioapic.reg);

		d = 0;

		switch (ioapic.reg) {
		case IOAPIC_ID: d = ioapic.id; break;
		case IOAPIC_VER: d = 0x170011; break;
		default:
			log_warnx("%s: unknown register id 0x%x", __func__,
			    ioapic.reg);
		}

		*data &= 0xFFFFFFFF00000000ULL;
		*data |= d;
	} else if (dir == MMIO_DIR_WRITE) {
		log_warnx("%s: requested write to register 0x%x, data=0x%x",
		    __func__, ioapic.reg, (uint32_t)*data);
	} else {
		log_warnx("%s: impossible direction %d", __func__, dir);
	}
}

static void
i82093aa_regsel(int dir, uint64_t *data)
{
	uint64_t d;

	log_warnx("%s: mmio op to register select (dir=%d)", __func__, dir);
	if (dir == MMIO_DIR_READ) {
		d = *data & 0xFFFFFFFF00000000ULL;
		log_warnx("%s: returning 0x%x", __func__, ioapic.reg);
		d |= ioapic.reg;
		*data = d;
	} else if (dir == MMIO_DIR_WRITE) {
		log_warnx("%s: setting regwin = 0x%x", __func__,
		    (uint32_t)*data);
		ioapic.reg = (uint32_t)*data;
	} else {
		log_warnx("%s: impossible direction %d", __func__, dir);
	}
}

int
i82093aa_mmio(int dir, paddr_t addr, uint64_t *data)
{
	log_warnx("%s: dir=%d addr=0x%lx data=0x%llx", __func__, dir, addr,
	    *data);

	if (addr == (IOAPIC_BASE_DEFAULT + IOAPIC_REG)) {
		i82093aa_regsel(dir, data);
	} else if (addr == (IOAPIC_BASE_DEFAULT + IOAPIC_DATA)) {
		i82093aa_winop(dir, data);
	} else {
		log_warnx("%s: invalid i82093aa register @ 0x%lx", __func__,
		    addr);
	}

	return 0;
}

void
i82093aa_init(void)
{
	mmio_dev_add(IOAPIC_BASE_DEFAULT, IOAPIC_BASE_DEFAULT + 0xFFFF,
	    (mmio_dev_fn_t)i82093aa_mmio);
}
