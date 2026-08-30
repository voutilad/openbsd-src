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
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <machine/armreg.h>
#include <machine/vmmvar.h>

#include <dev/vmm/vmm.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VMM_NODE	"/dev/vmm"
#define GUEST_MEM_SIZE	(4 * PAGE_SIZE)
#define GUEST_RESULT	0x42
#define VECTOR_BASE	PAGE_SIZE
#define IRQ_SPX_OFFSET	0x280

/* The guest remains in EL1h until another process raises its virtual IRQ. */
static const uint32_t guest_spin[] = {
	0x14000000,	/* b . */
};

/*
 * An IRQ taken from current EL using SP_EL1 selects the VBAR_EL1 + 0x280
 * slot.  Reaching HVC with the marker in x0 proves that the CPU took that
 * architectural vector; merely observing a host-side vCPU exit is not enough.
 */
static const uint32_t guest_irq[] = {
	0xd2800840,	/* mov x0, #GUEST_RESULT */
	0xd4000002,	/* hvc #0 */
};

static volatile sig_atomic_t alarm_fired;

static void
alarm_handler(int sig)
{
	(void)sig;
	alarm_fired = 1;
}

int
main(void)
{
	struct vm_create_params create;
	struct vm_exit vmexit;
	struct vm_intr_params intr;
	struct vm_resetcpu_params reset;
	struct vm_run_params run;
	struct vm_sharemem_params share;
	struct vm_terminate_params term;
	struct sigaction sa;
	char *mem;
	pid_t child = -1;
	int fd, status, ret = 1;

	fd = open(VMM_NODE, O_RDWR);
	if (fd == -1)
		err(1, "open %s", VMM_NODE);

	memset(&create, 0, sizeof(create));
	strlcpy(create.vcp_name, "inject", sizeof(create.vcp_name));
	create.vcp_ncpus = 1;
	create.vcp_nmemranges = 1;
	create.vcp_memranges[0].vmr_size = GUEST_MEM_SIZE;
	if (ioctl(fd, VMM_IOC_CREATE, &create) == -1)
		err(1, "VMM_IOC_CREATE");

	memset(&share, 0, sizeof(share));
	share.vsp_vm_id = create.vcp_id;
	share.vsp_nmemranges = create.vcp_nmemranges;
	memcpy(share.vsp_memranges, create.vcp_memranges,
	    sizeof(create.vcp_memranges));
	if (ioctl(fd, VMM_IOC_SHAREMEM, &share) == -1) {
		warn("VMM_IOC_SHAREMEM");
		goto out;
	}
	mem = (char *)create.vcp_memranges[0].vmr_va;
	memcpy(mem, guest_spin, sizeof(guest_spin));
	/* Each AArch64 vector slot is 128 bytes within the 2KB vector table. */
	memcpy(mem + VECTOR_BASE + IRQ_SPX_OFFSET, guest_irq,
	    sizeof(guest_irq));
	__builtin___clear_cache(mem, mem + GUEST_MEM_SIZE);

	memset(&reset, 0, sizeof(reset));
	reset.vrp_vm_id = create.vcp_id;
	reset.vrp_init_state.vrs_sp = GUEST_MEM_SIZE;
	/* Deliberately omit PSR_I so the asserted virtual IRQ is deliverable. */
	reset.vrp_init_state.vrs_pstate = PSR_F | PSR_A | PSR_D |
	    PSR_M_EL1h;
	reset.vrp_init_state.vrs_sctlr_el1 = SCTLR_RES1;
	reset.vrp_init_state.vrs_vbar_el1 = VECTOR_BASE;
	if (ioctl(fd, VMM_IOC_RESETCPU, &reset) == -1) {
		warn("VMM_IOC_RESETCPU");
		goto out;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = alarm_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL) == -1) {
		warn("sigaction");
		goto out;
	}

	child = fork();
	if (child == -1) {
		warn("fork");
		goto out;
	}
	if (child == 0) {
		/* Let the parent enter its busy guest before issuing the kick. */
		usleep(250000);
		memset(&intr, 0, sizeof(intr));
		intr.vip_vm_id = create.vcp_id;
		intr.vip_intr = 1;
		if (ioctl(fd, VMM_IOC_INTR, &intr) == -1)
			err(1, "VMM_IOC_INTR");
		_exit(0);
	}

	memset(&vmexit, 0, sizeof(vmexit));
	memset(&run, 0, sizeof(run));
	run.vrp_vm_id = create.vcp_id;
	run.vrp_exit = &vmexit;
	/* Host IRQ yields are transparent; only the guest's HVC ends the loop. */
	alarm(5);
	for (;;) {
		if (ioctl(fd, VMM_IOC_RUN, &run) == -1) {
			if (errno == EINTR && alarm_fired)
				break;
			warn("VMM_IOC_RUN");
			goto out;
		}
		if (run.vrp_exit_reason != VM_EXIT_NONE)
			break;
		if (alarm_fired)
			break;
	}
	alarm(0);
	if (alarm_fired) {
		warnx("guest did not take injected IRQ");
		goto out;
	}
	if (run.vrp_exit_reason != VM_EXIT_HVC) {
		warnx("unexpected exit reason 0x%04x: esr=0x%llx pc=0x%llx",
		    run.vrp_exit_reason, vmexit.vesr, vmexit.vrs.vrs_pc);
		goto out;
	}
	if (vmexit.vrs.vrs_gprs[VCPU_REGS_X0] != GUEST_RESULT) {
		warnx("IRQ vector did not run: x0=0x%llx pc=0x%llx",
		    vmexit.vrs.vrs_gprs[VCPU_REGS_X0], vmexit.vrs.vrs_pc);
		goto out;
	}
	if (waitpid(child, &status, 0) == -1) {
		warn("waitpid");
		goto out;
	}
	child = -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		warnx("interrupt injector failed");
		goto out;
	}

	/* Lower the level-triggered line just as a userland GIC eventually will. */
	memset(&intr, 0, sizeof(intr));
	intr.vip_vm_id = create.vcp_id;
	if (ioctl(fd, VMM_IOC_INTR, &intr) == -1) {
		warn("VMM_IOC_INTR clear");
		goto out;
	}
	printf("VMM_IOC_INTR vectored a running guest to its IRQ handler\n");
	ret = 0;

out:
	alarm(0);
	if (child != -1 && waitpid(child, &status, 0) == -1)
		warn("waitpid");
	memset(&term, 0, sizeof(term));
	term.vtp_vm_id = create.vcp_id;
	if (ioctl(fd, VMM_IOC_TERM, &term) == -1) {
		warn("VMM_IOC_TERM");
		ret = 1;
	}
	close(fd);
	return (ret);
}
