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
#include <sys/ioctl.h>

#include <machine/armreg.h>
#include <machine/vmmvar.h>
#include <dev/vmm/vmm.h>

#include <err.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MEM_SIZE	(4 * PAGE_SIZE)
#define INTID		33
#define PRIORITY		0x80

extern const char gicstate_start[], gicstate_end[];

struct guest {
	int fd;
	struct vm_run_params run;
	struct vm_exit exit;
};

static void
run_guest(struct guest *g)
{
	alarm(10);
	do {
		if (ioctl(g->fd, VMM_IOC_RUN, &g->run) == -1)
			err(1, "RUN");
	} while (g->run.vrp_exit_reason == VM_EXIT_NONE);
	alarm(0);
	if (g->run.vrp_exit_reason != VM_EXIT_HVC)
		errx(1, "expected HVC, reason=%x pc=%llx esr=%llx",
		    g->run.vrp_exit_reason, g->exit.vrs.vrs_pc, g->exit.vesr);
}

static void
reset_guest(struct guest *g)
{
	struct vm_resetcpu_params reset;

	memset(&reset, 0, sizeof(reset));
	reset.vrp_init_state.vrs_sp = MEM_SIZE;
	/* Poll IAR with IRQ exceptions masked; no vector table is needed. */
	reset.vrp_init_state.vrs_pstate = PSR_F | PSR_I | PSR_A | PSR_D |
	    PSR_M_EL1h;
	reset.vrp_init_state.vrs_sctlr_el1 = SCTLR_RES1;
	if (ioctl(g->fd, VMM_IOC_RESETCPU, &reset) == -1)
		err(1, "RESETCPU");
	memset(&g->exit, 0, sizeof(g->exit));
}

static void
create_guest(struct guest *g, const char *name)
{
	struct vm_create_params create;
	struct vm_sharemem_params share;
	char *mem;
	int fd;

	fd = open("/dev/vmm", O_RDWR);
	if (fd == -1)
		err(1, "open /dev/vmm");
	memset(&create, 0, sizeof(create));
	strlcpy(create.vcp_name, name, sizeof(create.vcp_name));
	create.vcp_ncpus = 1;
	create.vcp_nmemranges = 1;
	create.vcp_memranges[0].vmr_size = MEM_SIZE;
	if (ioctl(fd, VMM_IOC_CREATE, &create) == -1)
		err(1, "CREATE");
	close(fd);
	memset(g, 0, sizeof(*g));
	g->fd = create.vcp_fd;
	g->run.vrp_exit = &g->exit;
	memset(&share, 0, sizeof(share));
	if (ioctl(g->fd, VMM_IOC_SHAREMEM, &share) == -1)
		err(1, "SHAREMEM");
	mem = (char *)create.vcp_memranges[0].vmr_va;
	memcpy(mem, gicstate_start, gicstate_end - gicstate_start);
	__builtin___clear_cache(mem, mem + MEM_SIZE);
	reset_guest(g);
}

static void
check_priority(struct guest *g, unsigned int expected, const char *where)
{
	if (g->exit.vrs.vrs_gprs[VCPU_REGS_X2] != expected)
		errx(1, "%s: RPR=%#llx, expected=%#x", where,
		    g->exit.vrs.vrs_gprs[VCPU_REGS_X2], expected);
}

int
main(void)
{
	struct guest a, b;
	struct vm_intr_params intr;
	unsigned int i;

	create_guest(&a, "gicstate-a");
	create_guest(&b, "gicstate-b");
	for (i = 0; i < 32; i++) {
		reset_guest(&a);
		reset_guest(&b);
		run_guest(&a);
		check_priority(&a, 0xff, "fresh A");
		memset(&intr, 0, sizeof(intr));
		intr.vip_intr = INTID;
		intr.vip_priority = PRIORITY;
		intr.vip_level = VMM_INTR_LEVEL_HIGH;
		if (ioctl(a.fd, VMM_IOC_INTR, &intr) == -1)
			err(1, "assert A");
		run_guest(&a);
		if (a.exit.vrs.vrs_gprs[VCPU_REGS_X1] != INTID)
			errx(1, "A acknowledged INTID %llu",
			    a.exit.vrs.vrs_gprs[VCPU_REGS_X1]);
		check_priority(&a, PRIORITY, "active A");
		intr.vip_level = VMM_INTR_LEVEL_LOW;
		if (ioctl(a.fd, VMM_IOC_INTR, &intr) == -1)
			err(1, "lower A");
		/* A's active priority must neither leak to B nor vanish from A. */
		run_guest(&b);
		check_priority(&b, 0xff, "B while A is active");
		run_guest(&a);
		check_priority(&a, PRIORITY, "resumed active A");
		run_guest(&a);
		check_priority(&a, 0xff, "A after EOIR");
		/* RESETCPU must also clear priorities without relying on an EOI. */
		reset_guest(&a);
		run_guest(&a);
		intr.vip_level = VMM_INTR_LEVEL_HIGH;
		if (ioctl(a.fd, VMM_IOC_INTR, &intr) == -1)
			err(1, "assert before reset");
		run_guest(&a);
		check_priority(&a, PRIORITY, "active before reset");
		reset_guest(&a);
		run_guest(&a);
		check_priority(&a, 0xff, "reset active A");
	}
	close(a.fd);
	close(b.fd);
	printf("GIC_STATE_PASS: active priorities isolated across 32 VM "
	    "switches and resets\n");
	return (0);
}
