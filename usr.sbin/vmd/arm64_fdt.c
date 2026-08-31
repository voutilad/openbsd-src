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

#include <sys/param.h>
#include <sys/types.h>
#include <sys/endian.h>

#include <dev/ofw/fdt.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "arm64_vm.h"
#include "arm64_timer.h"

/*
 * An FDT has three variable-size areas after its header: a memory reservation
 * map, a tokenized tree, and a property-name string table.  The writer keeps
 * one cursor across those areas and fails instead of producing a partial blob
 * when the fixed firmware page is too small.
 */
struct fdt_writer {
	uint8_t	*buf;
	size_t	 size;
	size_t	 off;
};

/*
 * Using a structure of character arrays gives each property name a compile-
 * time string-table offset without relying on hand-maintained byte counts.
 * Character arrays have one-byte alignment, so no padding is introduced.
 */
struct arm64_fdt_strings {
	char	address_cells[sizeof("#address-cells")];
	char	size_cells[sizeof("#size-cells")];
	char	compatible[sizeof("compatible")];
	char	model[sizeof("model")];
	char	device_type[sizeof("device_type")];
	char	reg[sizeof("reg")];
	char	interrupt_cells[sizeof("#interrupt-cells")];
	char	interrupt_controller[sizeof("interrupt-controller")];
	char	interrupt_parent[sizeof("interrupt-parent")];
	char	interrupts[sizeof("interrupts")];
	char	interrupt_names[sizeof("interrupt-names")];
	char	phandle[sizeof("phandle")];
	char	stdout_path[sizeof("stdout-path")];
	char	status[sizeof("status")];
	char	clock_frequency[sizeof("clock-frequency")];
	char	physical_timer[sizeof("openbsd,physical-timer")];
};

static const struct arm64_fdt_strings arm64_fdt_strings = {
	.address_cells = "#address-cells",
	.size_cells = "#size-cells",
	.compatible = "compatible",
	.model = "model",
	.device_type = "device_type",
	.reg = "reg",
	.interrupt_cells = "#interrupt-cells",
	.interrupt_controller = "interrupt-controller",
	.interrupt_parent = "interrupt-parent",
	.interrupts = "interrupts",
	.interrupt_names = "interrupt-names",
	.phandle = "phandle",
	.stdout_path = "stdout-path",
	.status = "status",
	.clock_frequency = "clock-frequency",
	.physical_timer = "openbsd,physical-timer"
};

#define FDT_NAMEOFF(_member) \
	((uint32_t)offsetof(struct arm64_fdt_strings, _member))

static int	fdt_begin_node(struct fdt_writer *, const char *);
static int	fdt_end_node(struct fdt_writer *);
static int	fdt_prop(struct fdt_writer *, uint32_t, const void *,
		    size_t);
static int	fdt_prop_gic_reg(struct fdt_writer *);
static int	fdt_prop_reg(struct fdt_writer *, uint64_t, uint64_t);
static int	fdt_prop_string(struct fdt_writer *, uint32_t,
		    const char *);
static int	fdt_prop_u32(struct fdt_writer *, uint32_t, uint32_t);
static int	fdt_put(struct fdt_writer *, const void *, size_t);
static int	fdt_put_u32(struct fdt_writer *, uint32_t);
static int	fdt_put_u64(struct fdt_writer *, uint64_t);
static int	fdt_round(struct fdt_writer *, size_t);

static int
fdt_put(struct fdt_writer *w, const void *data, size_t len)
{
	if (len > w->size - w->off)
		return (-1);
	if (len != 0)
		memcpy(w->buf + w->off, data, len);
	w->off += len;
	return (0);
}

static int
fdt_put_u32(struct fdt_writer *w, uint32_t value)
{
	value = htobe32(value);
	return (fdt_put(w, &value, sizeof(value)));
}

static int
fdt_put_u64(struct fdt_writer *w, uint64_t value)
{
	value = htobe64(value);
	return (fdt_put(w, &value, sizeof(value)));
}

static int
fdt_round(struct fdt_writer *w, size_t align)
{
	size_t pad;

	pad = -w->off & (align - 1);
	if (pad > w->size - w->off)
		return (-1);
	/* The destination was initially cleared, so advancing emits zero pad. */
	w->off += pad;
	return (0);
}

static int
fdt_begin_node(struct fdt_writer *w, const char *name)
{
	if (fdt_put_u32(w, FDT_NODE_BEGIN) == -1 ||
	    fdt_put(w, name, strlen(name) + 1) == -1 ||
	    fdt_round(w, sizeof(uint32_t)) == -1)
		return (-1);
	return (0);
}

static int
fdt_end_node(struct fdt_writer *w)
{
	return (fdt_put_u32(w, FDT_NODE_END));
}

static int
fdt_prop(struct fdt_writer *w, uint32_t nameoff, const void *data,
    size_t len)
{
	if (len > UINT32_MAX || fdt_put_u32(w, FDT_PROPERTY) == -1 ||
	    fdt_put_u32(w, len) == -1 || fdt_put_u32(w, nameoff) == -1 ||
	    fdt_put(w, data, len) == -1 ||
	    fdt_round(w, sizeof(uint32_t)) == -1)
		return (-1);
	return (0);
}

static int
fdt_prop_u32(struct fdt_writer *w, uint32_t nameoff, uint32_t value)
{
	value = htobe32(value);
	return (fdt_prop(w, nameoff, &value, sizeof(value)));
}

static int
fdt_prop_reg(struct fdt_writer *w, uint64_t addr, uint64_t size)
{
	uint32_t cells[4];

	/* The root advertises two address cells and two size cells. */
	cells[0] = htobe32(addr >> 32);
	cells[1] = htobe32(addr);
	cells[2] = htobe32(size >> 32);
	cells[3] = htobe32(size);
	return (fdt_prop(w, FDT_NAMEOFF(reg), cells, sizeof(cells)));
}

static int
fdt_prop_gic_reg(struct fdt_writer *w)
{
	uint32_t cells[8];

	/* GICv3 requires a Distributor tuple followed by its Redistributor. */
	cells[0] = htobe32(ARM64_GICD_BASE >> 32);
	cells[1] = htobe32(ARM64_GICD_BASE);
	cells[2] = htobe32(ARM64_GICD_SIZE >> 32);
	cells[3] = htobe32(ARM64_GICD_SIZE);
	cells[4] = htobe32(ARM64_GICR_BASE >> 32);
	cells[5] = htobe32(ARM64_GICR_BASE);
	cells[6] = htobe32(ARM64_GICR_SIZE >> 32);
	cells[7] = htobe32(ARM64_GICR_SIZE);
	return (fdt_prop(w, FDT_NAMEOFF(reg), cells, sizeof(cells)));
}

static int
fdt_prop_string(struct fdt_writer *w, uint32_t nameoff, const char *value)
{
	return (fdt_prop(w, nameoff, value, strlen(value) + 1));
}

/*
 * Build the deliberately small platform description used by the first arm64
 * vmd backend.  There is one CPU, one contiguous RAM range, a GICv3 with one
 * Redistributor, a generic timer using the non-secure physical PPI, and one
 * polling PL011.  The UART has no interrupts property, so the table does not
 * claim interrupt-driven serial I/O before it exists.
 *
 * The last 16KB stage-2 page is firmware-owned.  It contains this DTB and is
 * omitted from /memory so a future guest allocator cannot recycle the blob.
 * The reservation entry documents the same ownership for FDT consumers that
 * process the legacy reservation map.
 */
int
arm64_fdt_build(void *buf, size_t buflen, size_t ram_size, size_t *sizep)
{
	static const char uart_compat[] = "arm,sbsa-uart\0arm,pl011";
	struct fdt_writer w;
	struct fdt_head header;
	uint32_t cpu_reg[2] = { 0, 0 };
	uint32_t timer_interrupt[3];
	uint64_t dtb_gpa, usable_ram;
	size_t strings_off, struct_off, struct_size;

	if (buf == NULL || sizep == NULL || buflen < sizeof(header) ||
	    ram_size <= ARM64_FDT_SIZE ||
	    ARM64_RAM_BASE > UINT64_MAX - ram_size) {
		errno = EINVAL;
		return (-1);
	}
	dtb_gpa = ARM64_RAM_BASE + ram_size - ARM64_FDT_SIZE;
	usable_ram = ram_size - ARM64_FDT_SIZE;

	memset(buf, 0, buflen);
	w.buf = buf;
	w.size = buflen;
	w.off = sizeof(header);

	/* Reserve the complete firmware page, then terminate with a zero pair. */
	if (fdt_put_u64(&w, dtb_gpa) == -1 ||
	    fdt_put_u64(&w, ARM64_FDT_SIZE) == -1 ||
	    fdt_put_u64(&w, 0) == -1 || fdt_put_u64(&w, 0) == -1)
		goto nospc;
	struct_off = w.off;

	if (fdt_begin_node(&w, "") == -1 ||
	    fdt_prop_u32(&w, FDT_NAMEOFF(address_cells), 2) == -1 ||
	    fdt_prop_u32(&w, FDT_NAMEOFF(size_cells), 2) == -1 ||
	    fdt_prop_string(&w, FDT_NAMEOFF(model),
	    "OpenBSD vmd arm64") == -1 ||
	    fdt_prop_string(&w, FDT_NAMEOFF(compatible),
	    "openbsd,vmd-arm64") == -1 ||
	    fdt_prop_u32(&w, FDT_NAMEOFF(interrupt_parent), 1) == -1)
		goto nospc;

	if (fdt_begin_node(&w, "chosen") == -1 ||
	    fdt_prop_string(&w, FDT_NAMEOFF(stdout_path),
	    "/uart@9000000") == -1 || fdt_end_node(&w) == -1)
		goto nospc;

	if (fdt_begin_node(&w, "memory@40000000") == -1 ||
	    fdt_prop_string(&w, FDT_NAMEOFF(device_type), "memory") == -1 ||
	    fdt_prop_reg(&w, ARM64_RAM_BASE, usable_ram) == -1 ||
	    fdt_end_node(&w) == -1)
		goto nospc;

	if (fdt_begin_node(&w, "cpus") == -1 ||
	    fdt_prop_u32(&w, FDT_NAMEOFF(address_cells), 2) == -1 ||
	    fdt_prop_u32(&w, FDT_NAMEOFF(size_cells), 0) == -1 ||
	    fdt_begin_node(&w, "cpu@0") == -1 ||
	    fdt_prop_string(&w, FDT_NAMEOFF(device_type), "cpu") == -1 ||
	    fdt_prop_string(&w, FDT_NAMEOFF(compatible), "arm,arm-v8") == -1 ||
	    fdt_prop(&w, FDT_NAMEOFF(reg), cpu_reg, sizeof(cpu_reg)) == -1 ||
	    fdt_prop_string(&w, FDT_NAMEOFF(status), "okay") == -1 ||
	    fdt_end_node(&w) == -1 || fdt_end_node(&w) == -1)
		goto nospc;

	/* Phandle 1 is also the root's default interrupt-parent above. */
	if (fdt_begin_node(&w, "intc@8000000") == -1 ||
	    fdt_prop_string(&w, FDT_NAMEOFF(compatible), "arm,gic-v3") == -1 ||
	    fdt_prop_u32(&w, FDT_NAMEOFF(interrupt_cells), 3) == -1 ||
	    fdt_prop(&w, FDT_NAMEOFF(interrupt_controller), NULL, 0) == -1 ||
	    fdt_prop_gic_reg(&w) == -1 ||
	    fdt_prop_u32(&w, FDT_NAMEOFF(phandle), 1) == -1 ||
	    fdt_prop_string(&w, FDT_NAMEOFF(status), "okay") == -1 ||
	    fdt_end_node(&w) == -1)
		goto nospc;

	/* GIC PPI 14 is architectural INTID 30, the non-secure physical timer. */
	timer_interrupt[0] = htobe32(1);
	timer_interrupt[1] = htobe32(ARM64_TIMER_INTID - 16);
	timer_interrupt[2] = htobe32(4);
	if (fdt_begin_node(&w, "timer") == -1 ||
	    fdt_prop_string(&w, FDT_NAMEOFF(compatible),
	    "arm,armv8-timer") == -1 ||
	    fdt_prop(&w, FDT_NAMEOFF(interrupts), timer_interrupt,
	    sizeof(timer_interrupt)) == -1 ||
	    fdt_prop_string(&w, FDT_NAMEOFF(interrupt_names), "phys") == -1 ||
	    fdt_prop_u32(&w, FDT_NAMEOFF(clock_frequency),
	    ARM64_TIMER_FREQUENCY) == -1 ||
	    fdt_prop(&w, FDT_NAMEOFF(physical_timer), NULL, 0) == -1 ||
	    fdt_end_node(&w) == -1)
		goto nospc;

	if (fdt_begin_node(&w, "uart@9000000") == -1 ||
	    fdt_prop(&w, FDT_NAMEOFF(compatible), uart_compat,
	    sizeof(uart_compat)) == -1 ||
	    fdt_prop_reg(&w, ARM64_UART_BASE, 0x1000) == -1 ||
	    fdt_prop_string(&w, FDT_NAMEOFF(status), "okay") == -1 ||
	    fdt_end_node(&w) == -1 || fdt_end_node(&w) == -1 ||
	    fdt_put_u32(&w, FDT_END) == -1)
		goto nospc;
	struct_size = w.off - struct_off;

	strings_off = w.off;
	if (fdt_put(&w, &arm64_fdt_strings,
	    sizeof(arm64_fdt_strings)) == -1 || w.off > UINT32_MAX)
		goto nospc;

	memset(&header, 0, sizeof(header));
	header.fh_magic = htobe32(FDT_MAGIC);
	header.fh_size = htobe32(w.off);
	header.fh_struct_off = htobe32(struct_off);
	header.fh_strings_off = htobe32(strings_off);
	header.fh_reserve_off = htobe32(sizeof(header));
	header.fh_version = htobe32(FDT_CODE_VERSION);
	header.fh_comp_ver = htobe32(16);
	header.fh_boot_cpu_id = htobe32(0);
	header.fh_strings_size = htobe32(sizeof(arm64_fdt_strings));
	header.fh_struct_size = htobe32(struct_size);
	memcpy(buf, &header, sizeof(header));
	*sizep = w.off;
	return (0);

nospc:
	errno = ENOSPC;
	return (-1);
}
