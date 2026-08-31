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
 *
 * Minimal freestanding arm64 payload for the vmd FDT and PL011 regression.
 * It intentionally uses no OpenBSD headers or runtime so guest.elf is a
 * fixed-address payload that vmd's first arm64 loader can copy directly.
 */

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;
typedef unsigned long usize;

#define FDT_MAGIC	0xd00dfeedU
#define FDT_NODE_BEGIN	1
#define FDT_NODE_END	2
#define FDT_PROPERTY	3
#define FDT_NOP		4
#define FDT_END		9

#define FDT_HEADER_SIZE	40
#define FDT_MAX_SIZE	16384
#define FDT_OFF_SIZE	4
#define FDT_OFF_STRUCT	8
#define FDT_OFF_STRINGS	12
#define FDT_OFF_VERSION	20
#define FDT_OFF_STRSIZE	32
#define FDT_OFF_STRUCTSIZE 36

#define PL011_DR	0x00
#define PL011_FR	0x18
#define PL011_FR_TXFF	(1U << 5)
#define FALLBACK_UART	0x09000000UL

struct fdt_walk {
	const u8 *blob;
	const u8 *p;
	const u8 *end;
	const u8 *strings;
	const u8 *strings_end;
	int depth;
};

struct fdt_item {
	u32 token;
	const u8 *name;
	const u8 *data;
	u32 len;
	int depth;
};

static int	blob_init(struct fdt_walk *, const void *);
static int	bytes_equal(const u8 *, usize, const char *);
static u32	get_be32(const void *);
static u64	find_uart(const void *);
static int	property_has_string(const u8 *, u32, const char *);
static int	string_equal(const u8 *, const char *);
static int	walk_next(struct fdt_walk *, struct fdt_item *);
static void	uart_puts(u64, const char *);

void	guest_main(const void *);

static u32
get_be32(const void *vp)
{
	const u8 *p = vp;

	return ((u32)p[0] << 24 | (u32)p[1] << 16 |
	    (u32)p[2] << 8 | p[3]);
}

static int
bytes_equal(const u8 *value, usize len, const char *expected)
{
	usize i;

	for (i = 0; i < len; i++) {
		if (expected[i] == '\0' || value[i] != (u8)expected[i])
			return (0);
	}
	return (expected[len] == '\0');
}

static int
string_equal(const u8 *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		if (*a++ != (u8)*b++)
			return (0);
	}
	return (*a == '\0' && *b == '\0');
}

static int
blob_init(struct fdt_walk *walk, const void *fdt)
{
	const u8 *blob = fdt;
	u32 size, strings_off, strings_size, struct_off, struct_size;

	if (blob == (const void *)0 || get_be32(blob) != FDT_MAGIC)
		return (-1);
	size = get_be32(blob + FDT_OFF_SIZE);
	struct_off = get_be32(blob + FDT_OFF_STRUCT);
	strings_off = get_be32(blob + FDT_OFF_STRINGS);
	strings_size = get_be32(blob + FDT_OFF_STRSIZE);
	struct_size = get_be32(blob + FDT_OFF_STRUCTSIZE);
	if (size < FDT_HEADER_SIZE || size > FDT_MAX_SIZE ||
	    get_be32(blob + FDT_OFF_VERSION) < 17 ||
	    struct_off > size || struct_size > size - struct_off ||
	    strings_off > size || strings_size > size - strings_off)
		return (-1);

	walk->blob = blob;
	walk->p = blob + struct_off;
	walk->end = walk->p + struct_size;
	walk->strings = blob + strings_off;
	walk->strings_end = walk->strings + strings_size;
	walk->depth = -1;
	return (0);
}

/*
 * Return one structure token at a time.  All FDT integers are big-endian and
 * node names and property values are padded to four bytes.  Bounds checks
 * make a malformed table fail closed before the payload performs MMIO.
 */
static int
walk_next(struct fdt_walk *walk, struct fdt_item *item)
{
	const u8 *name, *nul;
	usize off;
	u32 len, nameoff, token;

	for (;;) {
		if ((usize)(walk->end - walk->p) < sizeof(u32))
			return (-1);
		token = get_be32(walk->p);
		walk->p += sizeof(u32);
		item->token = token;
		item->name = (const void *)0;
		item->data = (const void *)0;
		item->len = 0;
		item->depth = walk->depth;

		switch (token) {
		case FDT_NODE_BEGIN:
			name = walk->p;
			for (nul = name; nul < walk->end && *nul != '\0'; nul++)
				;
			if (nul == walk->end)
				return (-1);
			off = (usize)(nul + 1 - walk->blob);
			off = (off + 3) & ~3UL;
			if (off > (usize)(walk->end - walk->blob))
				return (-1);
			walk->p = walk->blob + off;
			item->name = name;
			item->depth = ++walk->depth;
			return (1);
		case FDT_NODE_END:
			if (walk->depth < 0)
				return (-1);
			item->depth = walk->depth--;
			return (1);
		case FDT_PROPERTY:
			if ((usize)(walk->end - walk->p) < 2 * sizeof(u32))
				return (-1);
			len = get_be32(walk->p);
			nameoff = get_be32(walk->p + sizeof(u32));
			walk->p += 2 * sizeof(u32);
			if (len > (usize)(walk->end - walk->p) ||
			    nameoff >= (usize)(walk->strings_end - walk->strings))
				return (-1);
			name = walk->strings + nameoff;
			for (nul = name; nul < walk->strings_end && *nul != '\0';
			    nul++)
				;
			if (nul == walk->strings_end)
				return (-1);
			item->name = name;
			item->data = walk->p;
			item->len = len;
			off = (usize)(walk->p - walk->blob) + len;
			off = (off + 3) & ~3UL;
			if (off > (usize)(walk->end - walk->blob))
				return (-1);
			walk->p = walk->blob + off;
			return (1);
		case FDT_NOP:
			continue;
		case FDT_END:
			return (0);
		default:
			return (-1);
		}
	}
}

static int
property_has_string(const u8 *data, u32 len, const char *expected)
{
	u32 off, slen;

	for (off = 0; off < len; off += slen + 1) {
		for (slen = 0; off + slen < len && data[off + slen] != '\0';
		    slen++)
			;
		if (off + slen == len)
			return (0);
		if (bytes_equal(data + off, slen, expected))
			return (1);
	}
	return (0);
}

static u64
find_uart(const void *fdt)
{
	struct fdt_item item;
	struct fdt_walk walk;
	const u8 *stdout_path = (const void *)0;
	u64 uart = 0;
	u32 i, stdout_len = 0;
	int compatible = 0, in_chosen = 0, in_uart = 0, rv;

	/* First obtain the console's absolute node path from /chosen. */
	if (blob_init(&walk, fdt) == -1)
		return (0);
	while ((rv = walk_next(&walk, &item)) > 0) {
		if (item.token == FDT_NODE_BEGIN && item.depth == 1)
			in_chosen = string_equal(item.name, "chosen");
		else if (item.token == FDT_NODE_END && item.depth == 1)
			in_chosen = 0;
		else if (item.token == FDT_PROPERTY && in_chosen &&
		    string_equal(item.name, "stdout-path") &&
		    item.len > 2 && item.data[0] == '/' &&
		    item.data[item.len - 1] == '\0') {
			stdout_path = item.data;
			stdout_len = item.len - 1;
		}
	}
	if (rv < 0 || stdout_path == (const void *)0)
		return (0);

	/* This minimal machine has only root-level devices; reject deeper paths. */
	for (i = 1; i < stdout_len; i++) {
		if (stdout_path[i] == '/')
			return (0);
	}

	/* Locate that node, verify its binding, and decode its 2+2-cell reg. */
	if (blob_init(&walk, fdt) == -1)
		return (0);
	while ((rv = walk_next(&walk, &item)) > 0) {
		if (item.token == FDT_NODE_BEGIN && item.depth == 1) {
			in_uart = string_equal(item.name,
			    (const char *)stdout_path + 1);
			compatible = 0;
			uart = 0;
		} else if (item.token == FDT_PROPERTY && in_uart &&
		    string_equal(item.name, "compatible")) {
			compatible = property_has_string(item.data, item.len,
			    "arm,pl011");
		} else if (item.token == FDT_PROPERTY && in_uart &&
		    string_equal(item.name, "reg") && item.len >= 16) {
			uart = (u64)get_be32(item.data) << 32 |
			    get_be32(item.data + 4);
		} else if (item.token == FDT_NODE_END && item.depth == 1) {
			if (in_uart && compatible && uart != 0)
				return (uart);
			in_uart = 0;
		}
	}
	return (0);
}

static void
uart_puts(u64 base, const char *s)
{
	volatile u32 *dr = (volatile u32 *)(base + PL011_DR);
	volatile u32 *fr = (volatile u32 *)(base + PL011_FR);

	while (*s != '\0') {
		while ((*fr & PL011_FR_TXFF) != 0)
			;
		*dr = (u8)*s++;
	}
}

void
guest_main(const void *fdt)
{
	u64 uart;

	uart = find_uart(fdt);
	if (uart != 0)
		uart_puts(uart, "arm64 vmd FDT + polling PL011 works\r\n");
	else
		uart_puts(FALLBACK_UART, "arm64 vmd FDT invalid\r\n");
}
