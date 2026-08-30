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

#include <machine/armreg.h>
#include <machine/vmmvar.h>

#include <dev/vmm/vmm.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VMM_NODE	"/dev/vmm"
#define GUEST_RESULT	0x42
#define GUEST_MEM_SIZE	(4 * PAGE_SIZE)
#define FAULT_GPA	GUEST_MEM_SIZE
#define HPFAR_FIPA_MASK	0xfffffffff0UL

/* mov x0, #GUEST_RESULT; ldr x2, [x1]; b . */
static const uint32_t guest_code[] = {
	0xd2800840,
	0xf9400022,
	0x14000000,
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
	struct vm_create_params vcp;
	struct vm_exit *exit;
	struct vm_resetcpu_params reset;
	struct vm_run_params run;
	struct vm_sharemem_params share;
	struct vm_terminate_params term;
	struct sigaction sa;
	int fd, ret = 1;

	fd = open(VMM_NODE, O_RDWR);
	if (fd == -1)
		err(1, "open %s", VMM_NODE);

	memset(&vcp, 0, sizeof(vcp));
	strlcpy(vcp.vcp_name, "regress", sizeof(vcp.vcp_name));
	vcp.vcp_ncpus = 1;
	vcp.vcp_nmemranges = 1;
	vcp.vcp_memranges[0].vmr_gpa = 0;
	vcp.vcp_memranges[0].vmr_size = GUEST_MEM_SIZE;
	if (ioctl(fd, VMM_IOC_CREATE, &vcp) == -1)
		err(1, "VMM_IOC_CREATE");

	memset(&share, 0, sizeof(share));
	share.vsp_vm_id = vcp.vcp_id;
	share.vsp_nmemranges = vcp.vcp_nmemranges;
	memcpy(share.vsp_memranges, vcp.vcp_memranges,
	    sizeof(vcp.vcp_memranges));
	if (ioctl(fd, VMM_IOC_SHAREMEM, &share) == -1)
		err(1, "VMM_IOC_SHAREMEM");
	memcpy((void *)vcp.vcp_memranges[0].vmr_va, guest_code,
	    sizeof(guest_code));
	__builtin___clear_cache((char *)vcp.vcp_memranges[0].vmr_va,
	    (char *)vcp.vcp_memranges[0].vmr_va + sizeof(guest_code));

	memset(&reset, 0, sizeof(reset));
	reset.vrp_vm_id = vcp.vcp_id;
	reset.vrp_vcpu_id = 0;
	reset.vrp_init_state.vrs_pc = 0;
	reset.vrp_init_state.vrs_sp = PAGE_SIZE;
	reset.vrp_init_state.vrs_gprs[VCPU_REGS_X1] = FAULT_GPA;
	reset.vrp_init_state.vrs_pstate = PSR_F | PSR_I | PSR_A | PSR_D |
	    PSR_M_EL1h;
	reset.vrp_init_state.vrs_sctlr_el1 = SCTLR_RES1;
	if (ioctl(fd, VMM_IOC_RESETCPU, &reset) == -1) {
		warn("VMM_IOC_RESETCPU");
		goto out;
	}

	exit = calloc(1, sizeof(*exit));
	if (exit == NULL)
		err(1, "calloc");
	memset(&run, 0, sizeof(run));
	run.vrp_vm_id = vcp.vcp_id;
	run.vrp_vcpu_id = 0;
	run.vrp_exit = exit;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = alarm_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL) == -1) {
		warn("sigaction");
		goto out_free;
	}
	alarm(2);
	do {
		if (ioctl(fd, VMM_IOC_RUN, &run) == -1) {
			if (errno == EINTR && alarm_fired)
				break;
			warn("VMM_IOC_RUN");
			goto out_free;
		}
	} while (run.vrp_exit_reason == VM_EXIT_NONE);
	alarm(0);
	if (alarm_fired) {
		warnx("guest failed to fault: pc=0x%llx x0=0x%llx "
		    "x1=0x%llx", exit->vrs.vrs_pc,
		    exit->vrs.vrs_gprs[VCPU_REGS_X0],
		    exit->vrs.vrs_gprs[VCPU_REGS_X1]);
		goto out_free;
	}

	if (run.vrp_exit_reason != VM_EXIT_EXCEPTION) {
		warnx("unexpected exit reason 0x%04x: esr=0x%llx "
		    "far=0x%llx hpfar=0x%llx pc=0x%llx",
		    run.vrp_exit_reason, exit->vesr, exit->vfar,
		    exit->vhpfar, exit->vrs.vrs_pc);
		goto out_free;
	}
	if (ESR_ELx_EXCEPTION(exit->vesr) != 0x24) {
		warnx("unexpected ESR_EL2 0x%llx", exit->vesr);
		goto out_free;
	}
	if (exit->vrs.vrs_gprs[VCPU_REGS_X0] != GUEST_RESULT) {
		warnx("unexpected x0 0x%llx",
		    exit->vrs.vrs_gprs[VCPU_REGS_X0]);
		goto out_free;
	}
	if (exit->vrs.vrs_pc != sizeof(uint32_t)) {
		warnx("unexpected pc 0x%llx", exit->vrs.vrs_pc);
		goto out_free;
	}
	if (((exit->vhpfar & HPFAR_FIPA_MASK) << 8 |
	    (exit->vfar & PAGE_MASK)) != FAULT_GPA) {
		warnx("unexpected fault address: far=0x%llx hpfar=0x%llx",
		    exit->vfar, exit->vhpfar);
		goto out_free;
	}

	printf("vcpu exited on stage-2 fault with x0=0x%llx pc=0x%llx\n",
	    exit->vrs.vrs_gprs[VCPU_REGS_X0], exit->vrs.vrs_pc);
	ret = 0;

out_free:
	free(exit);
out:
	memset(&term, 0, sizeof(term));
	term.vtp_vm_id = vcp.vcp_id;
	if (ioctl(fd, VMM_IOC_TERM, &term) == -1) {
		warn("VMM_IOC_TERM");
		ret = 1;
	}
	close(fd);
	return (ret);
}
