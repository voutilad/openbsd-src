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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VMM_NODE	"/dev/vmm"
#define GUEST_MEM_SIZE	(4 * PAGE_SIZE)
#define HOST_FP_PATTERN	0x1122334455667788ULL
#define GUEST_V8_PATTERN 0x5a5a5a5a5a5a5a5aULL
#define GUEST_V31_PATTERN 0xa5a5a5a5a5a5a5a5ULL
#define GUEST_FPCR	(1UL << 22)

/*
 * Enable FP at EL1, fill the low and high ends of the vector file, and set
 * FPCR before the first HVC.  Userland advances that HVC and re-enters.  The
 * guest then exports both vector values and FPCR through GPRs before taking
 * a second HVC.  This proves that private guest FP state survives a complete
 * exit even though VMM_IOC_READREGS intentionally exposes only GPR state.
 */
static const uint32_t guest_code[] = {
	0xd2800060,	/* mov x0, #3 */
	0xd36cac00,	/* lsl x0, x0, #20 */
	0xd5181040,	/* msr CPACR_EL1, x0 */
	0xd5033fdf,	/* isb */
	0x4f02e748,	/* movi v8.16b, #0x5a */
	0x4f05e4bf,	/* movi v31.16b, #0xa5 */
	0xd2a00803,	/* mov x3, #0x400000 */
	0xd51b4403,	/* msr FPCR, x3 */
	0xd2800020,	/* mov x0, #1 */
	0xd4000002,	/* hvc #0 */
	0x4e083d01,	/* mov x1, v8.d[0] */
	0x4e183fe2,	/* mov x2, v31.d[1] */
	0xd53b4403,	/* mrs x3, FPCR */
	0xd2800840,	/* mov x0, #0x42 */
	0xd4000002,	/* hvc #0 */
};

int run_with_fp(int, unsigned long, void *, uint64_t, uint64_t *);

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
	uint64_t observed;

	do {
		observed = 0;
		if (run_with_fp(fd, VMM_IOC_RUN, run, HOST_FP_PATTERN,
		    &observed) == -1) {
			if (errno == EINTR && alarm_fired)
				return (ETIMEDOUT);
			return (errno);
		}
		if (observed != HOST_FP_PATTERN) {
			warnx("host d8 corrupted: got 0x%llx, wanted 0x%llx",
			    observed, HOST_FP_PATTERN);
			return (EIO);
		}
	} while (run->vrp_exit_reason == VM_EXIT_NONE && !alarm_fired);

	return (alarm_fired ? ETIMEDOUT : 0);
}

int
main(void)
{
	struct vm_create_params create;
	struct vm_exit vmexit;
	struct vm_resetcpu_params reset;
	struct vm_run_params run;
	struct vm_rwregs_params write;
	struct vm_sharemem_params share;
	struct vm_terminate_params term;
	struct sigaction sa;
	char *mem;
	int error, fd, ret = 1;

	fd = open(VMM_NODE, O_RDWR);
	if (fd == -1)
		err(1, "open %s", VMM_NODE);

	memset(&create, 0, sizeof(create));
	strlcpy(create.vcp_name, "fp", sizeof(create.vcp_name));
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
		warnc(error, "run to first HVC");
		goto out_alarm;
	}
	if (run.vrp_exit_reason != VM_EXIT_HVC ||
	    vmexit.vrs.vrs_pc != 9 * sizeof(uint32_t)) {
		warnx("unexpected first exit: reason 0x%04x pc 0x%llx",
		    run.vrp_exit_reason, vmexit.vrs.vrs_pc);
		goto out_alarm;
	}

	/* HVC leaves ELR_EL2 on the trapping instruction; emulate completion. */
	vmexit.vrs.vrs_pc += sizeof(uint32_t);
	memset(&write, 0, sizeof(write));
	write.vrwp_vm_id = create.vcp_id;
	write.vrwp_mask = VM_RWREGS_ALL;
	memcpy(&write.vrwp_regs, &vmexit.vrs, sizeof(write.vrwp_regs));
	if (ioctl(fd, VMM_IOC_WRITEREGS, &write) == -1) {
		warn("VMM_IOC_WRITEREGS");
		goto out_alarm;
	}

	if ((error = run_to_exit(fd, &run)) != 0) {
		warnc(error, "run to second HVC");
		goto out_alarm;
	}
	if (run.vrp_exit_reason != VM_EXIT_HVC ||
	    vmexit.vrs.vrs_pc != 14 * sizeof(uint32_t) ||
	    vmexit.vrs.vrs_gprs[VCPU_REGS_X0] != 0x42 ||
	    vmexit.vrs.vrs_gprs[VCPU_REGS_X1] != GUEST_V8_PATTERN ||
	    vmexit.vrs.vrs_gprs[VCPU_REGS_X2] != GUEST_V31_PATTERN ||
	    vmexit.vrs.vrs_gprs[VCPU_REGS_X3] != GUEST_FPCR) {
		warnx("guest FP state lost: reason 0x%04x pc 0x%llx "
		    "x0 0x%llx x1 0x%llx x2 0x%llx fpcr 0x%llx",
		    run.vrp_exit_reason, vmexit.vrs.vrs_pc,
		    vmexit.vrs.vrs_gprs[VCPU_REGS_X0],
		    vmexit.vrs.vrs_gprs[VCPU_REGS_X1],
		    vmexit.vrs.vrs_gprs[VCPU_REGS_X2],
		    vmexit.vrs.vrs_gprs[VCPU_REGS_X3]);
		goto out_alarm;
	}

	printf("preserved host and guest FP/AdvSIMD state across vCPU exits\n");
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
