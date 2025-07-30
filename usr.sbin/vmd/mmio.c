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

#include <errno.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/queue.h>

#include "mmio.h"
#include "vmd.h"

SLIST_HEAD(mmio_dev_head, mmio_dev) mmio_devs;

void
mmio_init(void)
{
	SLIST_INIT(&mmio_devs);
}

int
mmio_dev_add(paddr_t start, paddr_t end, mmio_dev_fn_t fn)
{
	struct mmio_dev *dev;

	dev = malloc(sizeof(*dev));
	if (!dev)
		return ENOMEM;

	dev->start = start;
	dev->end = end;
	dev->fn = fn;

	SLIST_INSERT_HEAD(&mmio_devs, dev, dev_next);
	log_debug("%s: added mmio handler for range [0x%lx - 0x%lx]",
	    __func__, start, end);

	return 0;
}

mmio_dev_fn_t
mmio_find_dev(paddr_t addr)
{
	struct mmio_dev *dev;

	SLIST_FOREACH(dev, &mmio_devs, dev_next) {
		if (addr >= dev->start &&
		    addr <= dev->end)
			return dev->fn;
	}

	return NULL;
}
