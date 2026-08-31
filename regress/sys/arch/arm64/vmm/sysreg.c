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
#include <string.h>
#include <unistd.h>

#define VMM_NODE	"/dev/vmm"
#define GUEST_MEM_SIZE	(4 * PAGE_SIZE)
#define READ_VALUE	0xa5
#define WRITE_VALUE	0x5a
#define GUEST_RESULT	0x42

/*
 * HCR_EL2.IMO redirects these ICC_PMR_EL1 accesses to the virtual CPU
 * interface.  ICH_HCR_EL2.TC must trap both instructions to vmm(4):
 *
 *     mrs x2, ICC_PMR_EL1
 *     msr ICC_PMR_EL1, x3
 *     mov x0, #GUEST_RESULT
 *     hvc #0
 */
static const uint32_t guest_code[] = {
	0xd5384602,
	0xd5184603,
	0xd2800840,
	0xd4000002,
};

static volatile sig_atomic_t alarm_fired;

static void
alarm_handler(int sig)
{
	(void)sig;
	alarm_fired = 1;
}

static int
run_to_exit(int fd, struct vm_run_params *run)
{
	do {
		if (ioctl(fd, VMM_IOC_RUN, run) == -1) {
			if (errno == EINTR && alarm_fired)
				return (ETIMEDOUT);
			return (errno);
		}
	} while (run->vrp_exit_reason == VM_EXIT_NONE && !alarm_fired);

	return (alarm_fired ? ETIMEDOUT : 0);
}

static int
check_pmr_exit(struct vm_run_params *run, uint64_t pc, u_int rt, int read)
{
	uint64_t esr = run->vrp_exit->vesr;

	if (run->vrp_exit_reason != VM_EXIT_EXCEPTION) {
		warnx("unexpected exit reason 0x%04x at pc 0x%llx",
		    run->vrp_exit_reason, run->vrp_exit->vrs.vrs_pc);
		return (EINVAL);
	}
	if (ESR_ELx_EXCEPTION(esr) != EXCP_MSR ||
	    (esr & ESR_ELx_IL) == 0 ||
	    ISS_MSR_OP0(esr) != 3 || ISS_MSR_OP1(esr) != 0 ||
	    ISS_MSR_CRn(esr) != 4 || ISS_MSR_CRm(esr) != 6 ||
	    ISS_MSR_OP2(esr) != 0 || ISS_MSR_Rt(esr) != rt ||
	    !!(esr & ISS_MSR_DIR) != read) {
		warnx("unexpected ICC_PMR_EL1 syndrome 0x%llx", esr);
		return (EINVAL);
	}
	if (run->vrp_exit->vrs.vrs_pc != pc) {
		warnx("unexpected trapped pc 0x%llx, wanted 0x%llx",
		    run->vrp_exit->vrs.vrs_pc, pc);
		return (EINVAL);
	}
	return (0);
}

static int
complete_sysreg(int fd, struct vm_run_params *run, int read,
    uint64_t value)
{
	struct vm_rwregs_params write;
	u_int rt = ISS_MSR_Rt(run->vrp_exit->vesr);

	/* A read supplies Rt; a write consumes it.  Both retire by advancing PC. */
	if (read && rt != 31)
		run->vrp_exit->vrs.vrs_gprs[rt] = value;
	run->vrp_exit->vrs.vrs_pc += sizeof(uint32_t);

	memset(&write, 0, sizeof(write));
	write.vrwp_vm_id = run->vrp_vm_id;
	write.vrwp_vcpu_id = run->vrp_vcpu_id;
	write.vrwp_mask = VM_RWREGS_ALL;
	memcpy(&write.vrwp_regs, &run->vrp_exit->vrs,
	    sizeof(write.vrwp_regs));
	if (ioctl(fd, VMM_IOC_WRITEREGS, &write) == -1)
		return (errno);
	return (0);
}

int
main(void)
{
	struct vm_create_params create;
	struct vm_exit vmexit;
	struct vm_resetcpu_params reset;
	struct vm_run_params run;
	struct vm_sharemem_params share;
	struct vm_terminate_params term;
	struct sigaction sa;
	char *mem;
	int error, fd, ret = 1;

	fd = open(VMM_NODE, O_RDWR);
	if (fd == -1)
		err(1, "open %s", VMM_NODE);

	memset(&create, 0, sizeof(create));
	strlcpy(create.vcp_name, "sysreg", sizeof(create.vcp_name));
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
	memcpy(mem, guest_code, sizeof(guest_code));
	__builtin___clear_cache(mem, mem + sizeof(guest_code));

	memset(&reset, 0, sizeof(reset));
	reset.vrp_vm_id = create.vcp_id;
	reset.vrp_init_state.vrs_sp = GUEST_MEM_SIZE;
	reset.vrp_init_state.vrs_gprs[VCPU_REGS_X3] = WRITE_VALUE;
	reset.vrp_init_state.vrs_pstate = PSR_F | PSR_I | PSR_A | PSR_D |
	    PSR_M_EL1h;
	reset.vrp_init_state.vrs_sctlr_el1 = SCTLR_RES1;
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
	memset(&vmexit, 0, sizeof(vmexit));
	memset(&run, 0, sizeof(run));
	run.vrp_vm_id = create.vcp_id;
	run.vrp_exit = &vmexit;
	alarm(5);

	if ((error = run_to_exit(fd, &run)) != 0) {
		warnc(error, "run to ICC_PMR_EL1 read");
		goto out_alarm;
	}
	if (check_pmr_exit(&run, 0, VCPU_REGS_X2, 1) != 0)
		goto out_alarm;
	if ((error = complete_sysreg(fd, &run, 1, READ_VALUE)) != 0) {
		warnc(error, "complete ICC_PMR_EL1 read");
		goto out_alarm;
	}

	if ((error = run_to_exit(fd, &run)) != 0) {
		warnc(error, "run to ICC_PMR_EL1 write");
		goto out_alarm;
	}
	if (check_pmr_exit(&run, sizeof(uint32_t), VCPU_REGS_X3, 0) != 0)
		goto out_alarm;
	if (vmexit.vrs.vrs_gprs[VCPU_REGS_X2] != READ_VALUE ||
	    vmexit.vrs.vrs_gprs[VCPU_REGS_X3] != WRITE_VALUE) {
		warnx("bad register values: x2=0x%llx x3=0x%llx",
		    vmexit.vrs.vrs_gprs[VCPU_REGS_X2],
		    vmexit.vrs.vrs_gprs[VCPU_REGS_X3]);
		goto out_alarm;
	}
	if ((error = complete_sysreg(fd, &run, 0, 0)) != 0) {
		warnc(error, "complete ICC_PMR_EL1 write");
		goto out_alarm;
	}

	if ((error = run_to_exit(fd, &run)) != 0) {
		warnc(error, "run to HVC");
		goto out_alarm;
	}
	if (run.vrp_exit_reason != VM_EXIT_HVC ||
	    vmexit.vrs.vrs_gprs[VCPU_REGS_X0] != GUEST_RESULT) {
		warnx("guest did not resume after ICC completion: reason 0x%04x "
		    "x0=0x%llx esr=0x%llx", run.vrp_exit_reason,
		    vmexit.vrs.vrs_gprs[VCPU_REGS_X0], vmexit.vesr);
		goto out_alarm;
	}

	printf("trapped and completed ICC_PMR_EL1 read/write accesses\n");
	ret = 0;

out_alarm:
	alarm(0);
out:
	memset(&term, 0, sizeof(term));
	term.vtp_vm_id = create.vcp_id;
	if (ioctl(fd, VMM_IOC_TERM, &term) == -1) {
		warn("VMM_IOC_TERM");
		ret = 1;
	}
	close(fd);
	return (ret);
}
