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
 * Minimal freestanding arm64 payload for the vmd FDT, GICv3, and PL011
 * regression.
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
#define PL011_IMSC	0x38
#define PL011_MIS	0x40
#define  PL011_INT_RX	(1U << 4)
#define  PL011_INT_TX	(1U << 5)
#define PL011_ICR	0x44
#define FALLBACK_UART	0x09000000UL

#define GICD_CTLR	0x0000
#define  GICD_CTLR_ENABLE_G1	((1U << 0) | (1U << 1))
#define  GICD_CTLR_ARE_NS	(1U << 4)
#define GICD_IGROUPR1	0x0084
#define GICD_ISENABLER1	0x0104
#define GICD_ICENABLER1	0x0184
#define GICD_ISPENDR1	0x0204
#define GICD_ICPENDR1	0x0284
#define GICD_ICACTIVER1	0x0384
#define GICD_IPRIORITYR	0x0400
#define GICD_ICFGR	0x0c00
#define GICD_IROUTER	0x6000
#define GICR_WAKER	0x00014
#define  GICR_WAKER_PROCESSOR_SLEEP (1U << 1)
#define GICR_IGROUPR0	0x10080
#define GICR_ISENABLER0	0x10100
#define GICR_IPRIORITYR	0x10400

#define TEST_INTID	33
#define TEST_INTID_BIT	(1U << (TEST_INTID - 32))
#define TIMER_INTID	27
#define TIMER_INTID_BIT	(1U << TIMER_INTID)
#define TIMER_TICKS	240000
#define GIC_SPURIOUS	1023

struct gic_bases {
	u64 dist;
	u64 redist;
};

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
static int	find_gic(const void *, struct gic_bases *);
static int	find_timer_intid(const void *);
static u64	find_uart(const void *);
static int	property_has_string(const u8 *, u32, const char *);
static int	string_equal(const u8 *, const char *);
static int	walk_next(struct fdt_walk *, struct fdt_item *);
static void	uart_puts(u64, const char *);

static volatile u32 guest_intid = GIC_SPURIOUS;
static volatile u32 guest_hold_spi;
static volatile u32 guest_uart_data;
static volatile u32 guest_uart_tx_irq;
static volatile u64 guest_held_iar;
static volatile u32 *guest_uart;
static volatile u32 *guest_dist;

void	guest_main(const void *);
void	guest_irq(void);

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

static int
find_gic(const void *fdt, struct gic_bases *bases)
{
	struct fdt_item item;
	struct fdt_walk walk;
	u64 dist = 0, dist_size = 0, redist = 0, redist_size = 0;
	int compatible = 0, in_gic = 0, rv;

	if (bases == (void *)0 || blob_init(&walk, fdt) == -1)
		return (-1);
	while ((rv = walk_next(&walk, &item)) > 0) {
		if (item.token == FDT_NODE_BEGIN && item.depth == 1) {
			in_gic = 1;
			compatible = 0;
			dist = dist_size = redist = redist_size = 0;
		} else if (item.token == FDT_PROPERTY && in_gic &&
		    string_equal(item.name, "compatible")) {
			compatible = property_has_string(item.data, item.len,
			    "arm,gic-v3");
		} else if (item.token == FDT_PROPERTY && in_gic &&
		    string_equal(item.name, "reg") && item.len >= 32) {
			dist = (u64)get_be32(item.data) << 32 |
			    get_be32(item.data + 4);
			dist_size = (u64)get_be32(item.data + 8) << 32 |
			    get_be32(item.data + 12);
			redist = (u64)get_be32(item.data + 16) << 32 |
			    get_be32(item.data + 20);
			redist_size = (u64)get_be32(item.data + 24) << 32 |
			    get_be32(item.data + 28);
		} else if (item.token == FDT_NODE_END && item.depth == 1) {
			if (in_gic && compatible && dist != 0 &&
			    dist_size >= 0x10000 && redist != 0 &&
			    redist_size >= 0x20000) {
				bases->dist = dist;
				bases->redist = redist;
				return (0);
			}
			in_gic = 0;
		}
	}
	return (-1);
}

static int
find_timer_intid(const void *fdt)
{
	struct fdt_item item;
	struct fdt_walk walk;
	u32 intid = 0;
	int compatible = 0, in_timer = 0, rv, virtual = 0;

	if (blob_init(&walk, fdt) == -1)
		return (-1);
	while ((rv = walk_next(&walk, &item)) > 0) {
		if (item.token == FDT_NODE_BEGIN && item.depth == 1) {
			in_timer = string_equal(item.name, "timer");
			compatible = virtual = 0;
			intid = 0;
		} else if (item.token == FDT_PROPERTY && in_timer &&
		    string_equal(item.name, "compatible")) {
			compatible = property_has_string(item.data, item.len,
			    "arm,armv8-timer");
		} else if (item.token == FDT_PROPERTY && in_timer &&
		    string_equal(item.name, "interrupt-names")) {
			virtual = property_has_string(item.data, item.len, "virt");
		} else if (item.token == FDT_PROPERTY && in_timer &&
		    string_equal(item.name, "interrupts") && item.len >= 12 &&
		    get_be32(item.data) == 1) {
			intid = 16 + get_be32(item.data + 4);
		} else if (item.token == FDT_NODE_END && item.depth == 1) {
			if (in_timer && compatible && virtual)
				return (intid);
			in_timer = 0;
		}
	}
	return (-1);
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

static void
write_icc_pmr(u64 value)
{
	__asm volatile("msr ICC_PMR_EL1, %x0" :: "r"(value) : "memory");
}

static void
write_icc_bpr1(u64 value)
{
	__asm volatile("msr ICC_BPR1_EL1, %x0" :: "r"(value) : "memory");
}

static void
write_icc_igrpen1(u64 value)
{
	__asm volatile("msr ICC_IGRPEN1_EL1, %x0" :: "r"(value) : "memory");
}

static u64
read_icc_iar1(void)
{
	u64 value;

	__asm volatile("mrs %x0, ICC_IAR1_EL1" : "=r"(value) :: "memory");
	return (value);
}

static void
write_icc_eoir1(u64 value)
{
	__asm volatile("msr ICC_EOIR1_EL1, %x0" :: "r"(value) : "memory");
}

static void
write_cntv_ctl(u64 value)
{
	__asm volatile("msr CNTV_CTL_EL0, %x0; isb" :: "r"(value) : "memory");
}

static void
write_cntv_tval(u64 value)
{
	__asm volatile("msr CNTV_TVAL_EL0, %x0; isb" :: "r"(value) : "memory");
}

void
guest_irq(void)
{
	u32 mis;
	u64 iar;

	/* IAR returns and activates the selected INTID; EOIR deactivates it. */
	iar = read_icc_iar1();
	guest_intid = iar & 0xffffff;
	if (guest_intid == TIMER_INTID)
		write_cntv_ctl(0);
	else if (guest_intid == TEST_INTID && guest_uart != (void *)0) {
		/*
		 * The hardware CPU-interface model has no maintenance interrupt with
		 * which to report an edge acknowledge back to the userland
		 * Distributor.  Explicitly clear this synthetic
		 * software-pended edge before EOI, exactly as the test would clear a
		 * device condition.  Real UART and timer inputs are level lines and
		 * deassert through their device models.
		 */
		guest_dist[GICD_ICPENDR1 / sizeof(u32)] = TEST_INTID_BIT;
		if (guest_hold_spi == 1) {
			/*
			 * Retain this interrupt as active in its List Register.  The
			 * higher-priority timer PPI programmed below must occupy another
			 * LR and preempt it without trapping IAR or EOIR to software.
			 */
			guest_held_iar = iar;
			guest_hold_spi = 2;
			return;
		}
		mis = guest_uart[PL011_MIS / sizeof(u32)];
		if ((mis & PL011_INT_RX) != 0) {
			guest_uart_data = guest_uart[PL011_DR / sizeof(u32)] & 0xff;
			guest_uart[PL011_ICR / sizeof(u32)] = PL011_INT_RX;
		}
		if ((mis & PL011_INT_TX) != 0) {
			guest_uart[PL011_IMSC / sizeof(u32)] &= ~PL011_INT_TX;
			guest_uart_tx_irq = 1;
		}
	}
	write_icc_eoir1(iar);
}

void
guest_main(const void *fdt)
{
	struct gic_bases gic;
	volatile u8 *priority, *rpriority;
	volatile u32 *dist, *redist;
	volatile u64 *router;
	u32 config, i, waker;
	u64 uart;

	uart = find_uart(fdt);
	if (uart == 0 || find_gic(fdt, &gic) == -1 ||
	    find_timer_intid(fdt) != TIMER_INTID) {
		uart_puts(FALLBACK_UART, "arm64 vmd FDT invalid\r\n");
		return;
	}
	dist = (volatile u32 *)gic.dist;
	guest_dist = dist;
	redist = (volatile u32 *)gic.redist;
	priority = (volatile u8 *)(gic.dist + GICD_IPRIORITYR);
	rpriority = (volatile u8 *)(gic.redist + GICR_IPRIORITYR);
	router = (volatile u64 *)(gic.dist + GICD_IROUTER);
	guest_uart = (volatile u32 *)uart;

	/* Wake the one Redistributor and clear all old SPI state. */
	waker = redist[GICR_WAKER / sizeof(u32)];
	redist[GICR_WAKER / sizeof(u32)] =
	    waker & ~GICR_WAKER_PROCESSOR_SLEEP;
	dist[GICD_ICENABLER1 / sizeof(u32)] = 0xffffffff;
	dist[GICD_ICPENDR1 / sizeof(u32)] = 0xffffffff;
	dist[GICD_ICACTIVER1 / sizeof(u32)] = 0xffffffff;

	/* Configure SPI 33 as a routed, non-secure Group-1 edge interrupt. */
	dist[GICD_IGROUPR1 / sizeof(u32)] |= TEST_INTID_BIT;
	priority[TEST_INTID] = 0x80;
	config = dist[(GICD_ICFGR + (TEST_INTID / 16) * sizeof(u32)) /
	    sizeof(u32)];
	config |= 2U << (2 * (TEST_INTID & 15));
	dist[(GICD_ICFGR + (TEST_INTID / 16) * sizeof(u32)) /
	    sizeof(u32)] = config;
	router[TEST_INTID] = 0;
	dist[GICD_ISENABLER1 / sizeof(u32)] = TEST_INTID_BIT;
	dist[GICD_CTLR / sizeof(u32)] |=
	    GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1;

	/* Open the system-register CPU interface, then unmask IRQ in PSTATE. */
	write_icc_pmr(0xff);
	write_icc_bpr1(0);
	write_icc_igrpen1(1);
	__asm volatile("dsb sy; isb; msr daifclr, #2" ::: "memory");

	/* A software-pended SPI exercises Distributor -> ICC -> IRQ -> EOI. */
	guest_intid = GIC_SPURIOUS;
	dist[GICD_ISPENDR1 / sizeof(u32)] = TEST_INTID_BIT;
	while (guest_intid == GIC_SPURIOUS)
		__asm volatile("wfi" ::: "memory");

	if (guest_intid == TEST_INTID)
		uart_puts(uart, "arm64 vmd FDT + GICv3 SPI interrupt works\r\n");
	else
		uart_puts(uart, "arm64 vmd GICv3 returned wrong INTID\r\n");

	/* Program the FDT-described virtual timer as a level-triggered PPI. */
	redist[GICR_IGROUPR0 / sizeof(u32)] |= TIMER_INTID_BIT;
	rpriority[TIMER_INTID] = 0x80;
	redist[GICR_ISENABLER0 / sizeof(u32)] = TIMER_INTID_BIT;
	guest_intid = GIC_SPURIOUS;
	write_cntv_ctl(0);
	write_cntv_tval(TIMER_TICKS);
	write_cntv_ctl(1);
	while (guest_intid == GIC_SPURIOUS)
		__asm volatile("wfi" ::: "memory");
	if (guest_intid == TIMER_INTID)
		uart_puts(uart, "arm64 vmd hardware virtual timer PPI works\r\n");
	else
		uart_puts(uart, "arm64 vmd timer returned wrong INTID\r\n");

	/*
	 * Re-arm through trapped WFI often enough to catch a lost wakeup or stale
	 * active LR.  A real kernel uses this sequence continuously for clock and
	 * sleep timeouts, rather than programming the timer only once at boot.
	 */
	for (i = 0; i < 32; i++) {
		guest_intid = GIC_SPURIOUS;
		write_cntv_ctl(0);
		write_cntv_tval(TIMER_TICKS);
		write_cntv_ctl(1);
		while (guest_intid == GIC_SPURIOUS)
			__asm volatile("wfi" ::: "memory");
		if (guest_intid != TIMER_INTID)
			break;
	}
	if (i == 32)
		uart_puts(uart, "arm64 vmd repeated timer wakeups work\r\n");
	else
		uart_puts(uart, "arm64 vmd repeated timer wakeup failed\r\n");

	/*
	 * Keep SPI 33 active in the virtual CPU interface, then require the
	 * higher-priority timer PPI to preempt it while ordinary instructions are
	 * executing.  This covers both a running-vCPU timer expiry and concurrent
	 * LR state, which the preceding WFI/sequential checks deliberately avoid.
	 */
	rpriority[TIMER_INTID] = 0x40;
	guest_intid = GIC_SPURIOUS;
	guest_hold_spi = 1;
	dist[GICD_ISPENDR1 / sizeof(u32)] = TEST_INTID_BIT;
	while (guest_hold_spi != 2)
		__asm volatile("wfi" ::: "memory");

	guest_intid = GIC_SPURIOUS;
	write_cntv_ctl(0);
	write_cntv_tval(TIMER_TICKS);
	write_cntv_ctl(1);
	while (guest_intid == GIC_SPURIOUS)
		__asm volatile("nop" ::: "memory");
	if (guest_intid == TIMER_INTID)
		uart_puts(uart, "arm64 vmd timer preempted an active SPI\r\n");
	else
		uart_puts(uart, "arm64 vmd timer preemption returned wrong INTID\r\n");
	write_icc_eoir1(guest_held_iar);
	guest_hold_spi = 0;
	rpriority[TIMER_INTID] = 0x80;

	/* Reconfigure SPI 33 as the level source described for the PL011. */
	config = dist[(GICD_ICFGR + (TEST_INTID / 16) * sizeof(u32)) /
	    sizeof(u32)];
	config &= ~(3U << (2 * (TEST_INTID & 15)));
	dist[(GICD_ICFGR + (TEST_INTID / 16) * sizeof(u32)) /
	    sizeof(u32)] = config;

	/* The empty transmitter is a level source while TXIM is enabled. */
	guest_intid = GIC_SPURIOUS;
	guest_uart_tx_irq = 0;
	guest_uart[PL011_IMSC / sizeof(u32)] = PL011_INT_TX;
	while (!guest_uart_tx_irq)
		__asm volatile("wfi" ::: "memory");
	uart_puts(uart, "arm64 vmd PL011 TX interrupt works\r\n");

	/* uart.sh supplies one byte through the PTY after seeing this marker. */
	guest_intid = GIC_SPURIOUS;
	guest_uart_data = 0;
	guest_uart[PL011_IMSC / sizeof(u32)] = PL011_INT_RX;
	uart_puts(uart, "arm64 vmd waiting for PL011 input\r\n");
	while (guest_uart_data == 0)
		__asm volatile("wfi" ::: "memory");
	guest_uart[PL011_IMSC / sizeof(u32)] = 0;
	if (guest_uart_data == 'x')
		uart_puts(uart, "arm64 vmd PL011 RX interrupt works\r\n");
	else
		uart_puts(uart, "arm64 vmd PL011 received wrong byte\r\n");
}
