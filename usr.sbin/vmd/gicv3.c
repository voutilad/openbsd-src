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

#include <machine/armreg.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "vmd.h"
#include "arm64_vm.h"
#include "gicv3.h"

/* Distributor registers. */
#define GICD_CTLR		0x0000
#define  GICD_CTLR_ENABLE_G1	((1U << 0) | (1U << 1))
#define  GICD_CTLR_ARE_NS	(1U << 4)
#define  GICD_CTLR_DS		(1U << 6)
#define GICD_TYPER		0x0004
#define GICD_IIDR		0x0008
#define GICD_SETSPI_NSR		0x0040
#define GICD_CLRSPI_NSR		0x0048
#define GICD_IGROUPR		0x0080
#define GICD_ISENABLER		0x0100
#define GICD_ICENABLER		0x0180
#define GICD_ISPENDR		0x0200
#define GICD_ICPENDR		0x0280
#define GICD_ISACTIVER		0x0300
#define GICD_ICACTIVER		0x0380
#define GICD_IPRIORITYR		0x0400
#define GICD_ICFGR		0x0c00
#define GICD_IGRPMODR		0x0d00
#define GICD_NSACR		0x0e00
#define GICD_IROUTER		0x6000

/* Redistributor registers, including its SGI/PPI frame at +64KB. */
#define GICR_CTLR		0x00000
#define GICR_IIDR		0x00004
#define GICR_TYPER		0x00008
#define  GICR_TYPER_LAST	(1ULL << 4)
#define GICR_WAKER		0x00014
#define  GICR_WAKER_CHILDREN_ASLEEP (1U << 2)
#define  GICR_WAKER_PROCESSOR_SLEEP (1U << 1)
#define GICR_IGROUPR0		0x10080
#define GICR_ISENABLER0		0x10100
#define GICR_ICENABLER0		0x10180
#define GICR_ISPENDR0		0x10200
#define GICR_ICPENDR0		0x10280
#define GICR_ISACTIVER0		0x10300
#define GICR_ICACTIVER0		0x10380
#define GICR_IPRIORITYR		0x10400
#define GICR_ICFGR0		0x10c00
#define GICR_ICFGR1		0x10c04
#define GICR_IGRPMODR0		0x10d00

#define GICV3_NINTIDS		64
#define GICV3_NREG32		(GICV3_NINTIDS / 32)
#define GICV3_NCONFIG		(GICV3_NINTIDS / 16)
#define GICV3_SPI_BASE		32
#define GICV3_SPURIOUS		1023
#define GICV3_IIDR_VALUE	0x0000043b
#define GICV3_CTLR_PRIBITS	(3U << 8)

struct gicv3_dev {
	pthread_mutex_t	 gd_mtx;
	uint32_t	 gd_vm_id;
	uint32_t	 gd_ctlr;
	uint32_t	 gd_group[GICV3_NREG32];
	uint32_t	 gd_enabled[GICV3_NREG32];
	uint32_t	 gd_pending[GICV3_NREG32];
	uint32_t	 gd_active[GICV3_NREG32];
	uint32_t	 gd_level[GICV3_NREG32];
	uint32_t	 gd_config[GICV3_NCONFIG];
	uint8_t		 gd_priority[GICV3_NINTIDS];
	uint64_t	 gd_route[GICV3_NINTIDS];
	uint32_t	 gd_waker;
	uint8_t		 gd_pmr;
	uint8_t		 gd_bpr1;
	uint8_t		 gd_igrpen1;
	uint8_t		 gd_irq_line;
};

static struct gicv3_dev gicv3 = {
	.gd_mtx = PTHREAD_MUTEX_INITIALIZER
};

static int	gicv3_candidate_locked(void);
static int	gicv3_dist_locked(paddr_t, size_t, int, uint64_t *);
static int	gicv3_drive_locked(void);
static int	gicv3_is_edge_locked(int);
static int	gicv3_priority_locked(paddr_t, int, size_t, int,
		    uint64_t *);
static int	gicv3_redist_locked(paddr_t, size_t, int, uint64_t *);
static uint8_t	gicv3_running_priority_locked(void);
static int	gicv3_sysreg_is(uint64_t, int, int, int, int, int);

static uint32_t
gicv3_bit(int intid)
{
	return (1U << (intid & 31));
}

static int
gicv3_word(int intid)
{
	return (intid >> 5);
}

void
gicv3_init(uint32_t vm_id)
{
	mutex_lock(&gicv3.gd_mtx);
	gicv3.gd_vm_id = vm_id;
	gicv3.gd_ctlr = GICD_CTLR_DS;
	memset(gicv3.gd_group, 0, sizeof(gicv3.gd_group));
	memset(gicv3.gd_enabled, 0, sizeof(gicv3.gd_enabled));
	memset(gicv3.gd_pending, 0, sizeof(gicv3.gd_pending));
	memset(gicv3.gd_active, 0, sizeof(gicv3.gd_active));
	memset(gicv3.gd_level, 0, sizeof(gicv3.gd_level));
	memset(gicv3.gd_config, 0, sizeof(gicv3.gd_config));
	memset(gicv3.gd_priority, 0xff, sizeof(gicv3.gd_priority));
	memset(gicv3.gd_route, 0, sizeof(gicv3.gd_route));
	gicv3.gd_waker = 0;
	gicv3.gd_pmr = 0;
	gicv3.gd_bpr1 = 0;
	gicv3.gd_igrpen1 = 0;
	gicv3.gd_irq_line = 0;
	mutex_unlock(&gicv3.gd_mtx);
}

/* Return the numerically highest-priority active interrupt. */
static uint8_t
gicv3_running_priority_locked(void)
{
	uint8_t priority = 0xff;
	int intid;

	for (intid = 0; intid < GICV3_NINTIDS; intid++) {
		if ((gicv3.gd_active[gicv3_word(intid)] &
		    gicv3_bit(intid)) != 0 &&
		    gicv3.gd_priority[intid] < priority)
			priority = gicv3.gd_priority[intid];
	}
	return (priority);
}

/*
 * Select the deliverable Group-1 interrupt with the highest priority.  The
 * first model has one vCPU, so a zero IROUTER affinity is its only route.
 * Binary-point priority grouping is intentionally deferred; comparing the
 * complete priority byte is sufficient for the initial non-nested case.
 */
static int
gicv3_candidate_locked(void)
{
	uint8_t best_priority = 0xff;
	uint8_t running_priority;
	uint32_t bit;
	int best = GICV3_SPURIOUS, intid, word;

	if ((gicv3.gd_ctlr & GICD_CTLR_ENABLE_G1) == 0 ||
	    gicv3.gd_igrpen1 == 0)
		return (GICV3_SPURIOUS);
	running_priority = gicv3_running_priority_locked();
	for (intid = 0; intid < GICV3_NINTIDS; intid++) {
		word = gicv3_word(intid);
		bit = gicv3_bit(intid);
		if ((gicv3.gd_group[word] & bit) == 0 ||
		    (gicv3.gd_enabled[word] & bit) == 0 ||
		    (gicv3.gd_pending[word] & bit) == 0 ||
		    (gicv3.gd_active[word] & bit) != 0 ||
		    gicv3.gd_priority[intid] >= gicv3.gd_pmr ||
		    gicv3.gd_priority[intid] >= running_priority)
			continue;
		if (intid >= GICV3_SPI_BASE && gicv3.gd_route[intid] != 0)
			continue;
		if (best == GICV3_SPURIOUS ||
		    gicv3.gd_priority[intid] < best_priority) {
			best = intid;
			best_priority = gicv3.gd_priority[intid];
		}
	}
	return (best);
}

/* Drive vmm(4)'s identity-free virtual IRQ input from the GIC state. */
static int
gicv3_drive_locked(void)
{
	int asserted, error;

	asserted = gicv3_candidate_locked() != GICV3_SPURIOUS;
	if (asserted == gicv3.gd_irq_line)
		return (0);
	error = vcpu_intr(gicv3.gd_vm_id, 0, asserted);
	if (error != 0)
		return (error);
	gicv3.gd_irq_line = asserted;
	if (asserted) {
		/* An asynchronous device may have pended the IRQ while WFI slept. */
		vcpu_unhalt(0);
		vcpu_signal_run(0);
	}
	return (0);
}

static int
gicv3_is_edge_locked(int intid)
{
	uint32_t shift;

	shift = 2 * (intid & 15) + 1;
	return ((gicv3.gd_config[intid >> 4] & (1U << shift)) != 0);
}

int
gicv3_set_irq(uint32_t vm_id, uint32_t vcpu_id, int intid, int asserted)
{
	uint32_t bit;
	int error, word;

	if (vm_id != gicv3.gd_vm_id || vcpu_id != 0 ||
	    intid < 0 || intid >= GICV3_NINTIDS)
		return (EINVAL);
	word = gicv3_word(intid);
	bit = gicv3_bit(intid);
	mutex_lock(&gicv3.gd_mtx);
	if (asserted) {
		gicv3.gd_level[word] |= bit;
		gicv3.gd_pending[word] |= bit;
	} else {
		gicv3.gd_level[word] &= ~bit;
		if (!gicv3_is_edge_locked(intid) &&
		    (gicv3.gd_active[word] & bit) == 0)
			gicv3.gd_pending[word] &= ~bit;
	}
	error = gicv3_drive_locked();
	mutex_unlock(&gicv3.gd_mtx);
	return (error);
}

/* Read or write a packed run of byte-sized priority registers. */
static int
gicv3_priority_locked(paddr_t offset, int first, size_t len, int write,
    uint64_t *data)
{
	uint64_t value = 0;
	size_t i;

	if (len != 1 && len != 4)
		return (EOPNOTSUPP);
	if (offset > GICV3_NINTIDS || len > GICV3_NINTIDS - offset)
		return (EFAULT);
	for (i = 0; i < len; i++) {
		if (write)
			gicv3.gd_priority[first + offset + i] = *data >> (i * 8);
		else
			value |= (uint64_t)gicv3.gd_priority[first + offset + i] <<
			    (i * 8);
	}
	if (!write)
		*data = value;
	return (0);
}

static int
gicv3_dist_locked(paddr_t offset, size_t len, int write, uint64_t *data)
{
	uint32_t index, value;
	int intid;

	if (offset >= GICD_IPRIORITYR &&
	    offset < GICD_IPRIORITYR + GICV3_NINTIDS)
		return (gicv3_priority_locked(offset - GICD_IPRIORITYR, 0,
		    len, write, data));
	if (offset >= GICD_IROUTER &&
	    offset < GICD_IROUTER + GICV3_NINTIDS * sizeof(uint64_t)) {
		if (len != sizeof(uint64_t) || (offset & 7) != 0)
			return (EOPNOTSUPP);
		intid = (offset - GICD_IROUTER) / sizeof(uint64_t);
		if (write)
			gicv3.gd_route[intid] = *data;
		else
			*data = gicv3.gd_route[intid];
		return (0);
	}
	if (len != sizeof(uint32_t) || (offset & 3) != 0)
		return (EOPNOTSUPP);
	value = *data;
	switch (offset) {
	case GICD_CTLR:
		if (write)
			gicv3.gd_ctlr = GICD_CTLR_DS | (value &
			    (GICD_CTLR_ENABLE_G1 | GICD_CTLR_ARE_NS));
		else
			*data = gicv3.gd_ctlr;
		return (0);
	case GICD_TYPER:
		if (!write)
			*data = GICV3_NREG32 - 1;
		return (0);
	case GICD_IIDR:
		if (!write)
			*data = GICV3_IIDR_VALUE;
		return (0);
	case GICD_SETSPI_NSR:
		if (write && value >= GICV3_SPI_BASE &&
		    value < GICV3_NINTIDS)
			gicv3.gd_pending[gicv3_word(value)] |= gicv3_bit(value);
		return (0);
	case GICD_CLRSPI_NSR:
		if (write && value >= GICV3_SPI_BASE &&
		    value < GICV3_NINTIDS)
			gicv3.gd_pending[gicv3_word(value)] &= ~gicv3_bit(value);
		return (0);
	}

#define GICD_REG32(_base, _member, _write) do { \
	if (offset >= (_base) && offset < (_base) + \
	    sizeof(gicv3._member)) { \
		index = (offset - (_base)) / sizeof(uint32_t); \
		if (write) { _write; } else *data = gicv3._member[index]; \
		return (0); \
	} \
} while (0)
	GICD_REG32(GICD_IGROUPR, gd_group,
	    gicv3.gd_group[index] = value);
	GICD_REG32(GICD_ISENABLER, gd_enabled,
	    gicv3.gd_enabled[index] |= value);
	GICD_REG32(GICD_ICENABLER, gd_enabled,
	    gicv3.gd_enabled[index] &= ~value);
	GICD_REG32(GICD_ISPENDR, gd_pending,
	    gicv3.gd_pending[index] |= value);
	GICD_REG32(GICD_ICPENDR, gd_pending,
	    gicv3.gd_pending[index] &= ~value);
	GICD_REG32(GICD_ISACTIVER, gd_active,
	    gicv3.gd_active[index] |= value);
	GICD_REG32(GICD_ICACTIVER, gd_active,
	    gicv3.gd_active[index] &= ~value);
	GICD_REG32(GICD_ICFGR, gd_config,
	    gicv3.gd_config[index] = value & 0xaaaaaaaa);
#undef GICD_REG32

	/* IGRPMODR and NSACR are RAZ/WI in this single-security-state model. */
	if ((offset >= GICD_IGRPMODR &&
	    offset < GICD_IGRPMODR + GICV3_NREG32 * sizeof(uint32_t)) ||
	    (offset >= GICD_NSACR && offset < GICD_NSACR +
	    (GICV3_NINTIDS / 16) * sizeof(uint32_t))) {
		if (!write)
			*data = 0;
		return (0);
	}

	/* Architecturally reserved registers in the aperture are RAZ/WI. */
	if (!write)
		*data = 0;
	return (0);
}

static int
gicv3_redist_locked(paddr_t offset, size_t len, int write, uint64_t *data)
{
	uint32_t value = *data;

	if (offset >= GICR_IPRIORITYR &&
	    offset < GICR_IPRIORITYR + GICV3_SPI_BASE)
		return (gicv3_priority_locked(offset - GICR_IPRIORITYR, 0,
		    len, write, data));
	if (offset == GICR_TYPER) {
		if (len != sizeof(uint64_t))
			return (EOPNOTSUPP);
		if (!write)
			*data = GICR_TYPER_LAST;
		return (0);
	}
	if (len != sizeof(uint32_t) || (offset & 3) != 0)
		return (EOPNOTSUPP);
	switch (offset) {
	case GICR_CTLR:
		if (!write)
			*data = 0;
		break;
	case GICR_IIDR:
		if (!write)
			*data = GICV3_IIDR_VALUE;
		break;
	case GICR_WAKER:
		if (write) {
			gicv3.gd_waker = value & GICR_WAKER_PROCESSOR_SLEEP;
			if (gicv3.gd_waker != 0)
				gicv3.gd_waker |= GICR_WAKER_CHILDREN_ASLEEP;
		} else
			*data = gicv3.gd_waker;
		break;
	case GICR_IGROUPR0:
		if (write)
			gicv3.gd_group[0] = value;
		else
			*data = gicv3.gd_group[0];
		break;
	case GICR_ISENABLER0:
		if (write)
			gicv3.gd_enabled[0] |= value;
		else
			*data = gicv3.gd_enabled[0];
		break;
	case GICR_ICENABLER0:
		if (write)
			gicv3.gd_enabled[0] &= ~value;
		else
			*data = gicv3.gd_enabled[0];
		break;
	case GICR_ISPENDR0:
		if (write)
			gicv3.gd_pending[0] |= value;
		else
			*data = gicv3.gd_pending[0];
		break;
	case GICR_ICPENDR0:
		if (write)
			gicv3.gd_pending[0] &= ~value;
		else
			*data = gicv3.gd_pending[0];
		break;
	case GICR_ISACTIVER0:
		if (write)
			gicv3.gd_active[0] |= value;
		else
			*data = gicv3.gd_active[0];
		break;
	case GICR_ICACTIVER0:
		if (write)
			gicv3.gd_active[0] &= ~value;
		else
			*data = gicv3.gd_active[0];
		break;
	case GICR_ICFGR0:
	case GICR_ICFGR1:
		if (write)
			gicv3.gd_config[(offset - GICR_ICFGR0) / 4] =
			    value & 0xaaaaaaaa;
		else
			*data = gicv3.gd_config[(offset - GICR_ICFGR0) / 4];
		break;
	case GICR_IGRPMODR0:
		if (!write)
			*data = 0;
		break;
	default:
		if (!write)
			*data = 0;
		break;
	}
	return (0);
}

int
gicv3_mmio(paddr_t gpa, size_t len, int write, uint64_t *data)
{
	int error;

	if (data == NULL)
		return (EINVAL);
	mutex_lock(&gicv3.gd_mtx);
	if (gpa >= ARM64_GICD_BASE &&
	    gpa - ARM64_GICD_BASE < ARM64_GICD_SIZE)
		error = gicv3_dist_locked(gpa - ARM64_GICD_BASE, len,
		    write, data);
	else if (gpa >= ARM64_GICR_BASE &&
	    gpa - ARM64_GICR_BASE < ARM64_GICR_SIZE)
		error = gicv3_redist_locked(gpa - ARM64_GICR_BASE, len,
		    write, data);
	else
		error = EFAULT;
	if (error == 0 && write)
		error = gicv3_drive_locked();
	mutex_unlock(&gicv3.gd_mtx);
	return (error);
}

static int
gicv3_sysreg_is(uint64_t esr, int op0, int op1, int crn, int crm, int op2)
{
	return (ISS_MSR_OP0(esr) == op0 && ISS_MSR_OP1(esr) == op1 &&
	    ISS_MSR_CRn(esr) == crn && ISS_MSR_CRm(esr) == crm &&
	    ISS_MSR_OP2(esr) == op2);
}

/*
 * Emulate the GICv3 system-register CPU interface selected by HCR_EL2.IMO.
 * ICH_HCR_EL2.TALL1 and TC make these accesses arrive as EXCP_MSR exits.
 * Reads of IAR acknowledge a pending interrupt; EOIR writes deactivate it.
 */
int
gicv3_icc(uint64_t esr, int write, uint64_t *data)
{
	uint32_t bit;
	int error = 0, intid, word;

	if (data == NULL)
		return (EINVAL);
	mutex_lock(&gicv3.gd_mtx);
	if (gicv3_sysreg_is(esr, 3, 0, 4, 6, 0)) { /* ICC_PMR_EL1 */
		if (write)
			gicv3.gd_pmr = *data;
		else
			*data = gicv3.gd_pmr;
	} else if (gicv3_sysreg_is(esr, 3, 0, 12, 12, 3)) { /* ICC_BPR1 */
		if (write)
			gicv3.gd_bpr1 = *data & 7;
		else
			*data = gicv3.gd_bpr1;
	} else if (gicv3_sysreg_is(esr, 3, 0, 12, 12, 4)) { /* ICC_CTLR */
		if (!write)
			*data = GICV3_CTLR_PRIBITS;
	} else if (gicv3_sysreg_is(esr, 3, 0, 12, 12, 5)) { /* ICC_SRE */
		if (!write)
			*data = 7;
	} else if (gicv3_sysreg_is(esr, 3, 0, 12, 12, 7)) { /* IGRPEN1 */
		if (write)
			gicv3.gd_igrpen1 = *data & 1;
		else
			*data = gicv3.gd_igrpen1;
	} else if (gicv3_sysreg_is(esr, 3, 0, 12, 12, 2)) { /* HPPIR1 */
		if (write)
			error = EINVAL;
		else
			*data = gicv3_candidate_locked();
	} else if (gicv3_sysreg_is(esr, 3, 0, 12, 11, 3)) { /* RPR */
		if (write)
			error = EINVAL;
		else
			*data = gicv3_running_priority_locked();
	} else if (gicv3_sysreg_is(esr, 3, 0, 12, 12, 0)) { /* IAR1 */
		if (write)
			error = EINVAL;
		else {
			intid = gicv3_candidate_locked();
			*data = intid;
			if (intid != GICV3_SPURIOUS) {
				word = gicv3_word(intid);
				bit = gicv3_bit(intid);
				gicv3.gd_pending[word] &= ~bit;
				gicv3.gd_active[word] |= bit;
			}
		}
	} else if (gicv3_sysreg_is(esr, 3, 0, 12, 12, 1) || /* EOIR1 */
	    gicv3_sysreg_is(esr, 3, 0, 12, 11, 1)) { /* DIR */
		if (!write)
			error = EINVAL;
		else {
			intid = *data;
			if (intid >= 0 && intid < GICV3_NINTIDS) {
				word = gicv3_word(intid);
				bit = gicv3_bit(intid);
				gicv3.gd_active[word] &= ~bit;
				if ((gicv3.gd_level[word] & bit) != 0)
					gicv3.gd_pending[word] |= bit;
			}
		}
	} else if (gicv3_sysreg_is(esr, 3, 0, 12, 11, 5)) { /* SGI1R */
		/* There is no remote target in the one-vCPU machine. */
		if (!write)
			error = EINVAL;
	} else
		error = ENOENT;
	if (error == 0)
		error = gicv3_drive_locked();
	mutex_unlock(&gicv3.gd_mtx);
	return (error);
}
