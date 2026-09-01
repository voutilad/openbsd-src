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

/* APIA, APIB, APDA, APDB, and APGA, with each low word followed by high. */
static const uint64_t guest_keys[] = {
	0x0101010101010101ULL, 0x0202020202020202ULL,
	0x1313131313131313ULL, 0x2424242424242424ULL,
	0x3535353535353535ULL, 0x4646464646464646ULL,
	0x5757575757575757ULL, 0x6868686868686868ULL,
	0x7979797979797979ULL, 0x8a8a8a8a8a8a8a8aULL,
};

/*
 * Change every EL1 pointer-authentication key and exit.  On re-entry, read
 * the keys into x11--x20 and exit again.  This checks both halves of all five
 * keys across a complete VMM_IOC_RUN round trip.  It also catches failure to
 * restore the host APIA key: OpenBSD return-address protection will reject
 * the host process's next authenticated return before this test can pass.
 */
static const uint32_t guest_code[] = {
	0xd5182101,	/* msr APIAKeyLo_EL1, x1 */
	0xd5182122,	/* msr APIAKeyHi_EL1, x2 */
	0xd5182143,	/* msr APIBKeyLo_EL1, x3 */
	0xd5182164,	/* msr APIBKeyHi_EL1, x4 */
	0xd5182205,	/* msr APDAKeyLo_EL1, x5 */
	0xd5182226,	/* msr APDAKeyHi_EL1, x6 */
	0xd5182247,	/* msr APDBKeyLo_EL1, x7 */
	0xd5182268,	/* msr APDBKeyHi_EL1, x8 */
	0xd5182309,	/* msr APGAKeyLo_EL1, x9 */
	0xd518232a,	/* msr APGAKeyHi_EL1, x10 */
	0xd5033fdf,	/* isb */
	0xd2800020,	/* mov x0, #1 */
	0xd4000002,	/* hvc #0 */
	0xd538210b,	/* mrs x11, APIAKeyLo_EL1 */
	0xd538212c,	/* mrs x12, APIAKeyHi_EL1 */
	0xd538214d,	/* mrs x13, APIBKeyLo_EL1 */
	0xd538216e,	/* mrs x14, APIBKeyHi_EL1 */
	0xd538220f,	/* mrs x15, APDAKeyLo_EL1 */
	0xd5382230,	/* mrs x16, APDAKeyHi_EL1 */
	0xd5382251,	/* mrs x17, APDBKeyLo_EL1 */
	0xd5382272,	/* mrs x18, APDBKeyHi_EL1 */
	0xd5382313,	/* mrs x19, APGAKeyLo_EL1 */
	0xd5382334,	/* mrs x20, APGAKeyHi_EL1 */
	0xd2800840,	/* mov x0, #0x42 */
	0xd4000002,	/* hvc #0 */
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
	uint64_t observed;
	size_t i;
	int error, fd, ret = 1;

	fd = open(VMM_NODE, O_RDWR);
	if (fd == -1)
		err(1, "open %s", VMM_NODE);

	memset(&create, 0, sizeof(create));
	strlcpy(create.vcp_name, "pauth", sizeof(create.vcp_name));
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
	for (i = 0; i < nitems(guest_keys); i++)
		reset.vrp_init_state.vrs_gprs[VCPU_REGS_X1 + i] = guest_keys[i];
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
	    vmexit.vrs.vrs_pc != 13 * sizeof(uint32_t) ||
	    vmexit.vrs.vrs_gprs[VCPU_REGS_X0] != 1) {
		warnx("unexpected first exit: reason 0x%04x pc 0x%llx x0 0x%llx",
		    run.vrp_exit_reason, vmexit.vrs.vrs_pc,
		    vmexit.vrs.vrs_gprs[VCPU_REGS_X0]);
		goto out_alarm;
	}

	if ((error = run_to_exit(fd, &run)) != 0) {
		warnc(error, "run to second HVC");
		goto out_alarm;
	}
	if (run.vrp_exit_reason != VM_EXIT_HVC ||
	    vmexit.vrs.vrs_pc != 25 * sizeof(uint32_t) ||
	    vmexit.vrs.vrs_gprs[VCPU_REGS_X0] != 0x42) {
		warnx("unexpected second exit: reason 0x%04x pc 0x%llx x0 0x%llx",
		    run.vrp_exit_reason, vmexit.vrs.vrs_pc,
		    vmexit.vrs.vrs_gprs[VCPU_REGS_X0]);
		goto out_alarm;
	}
	for (i = 0; i < nitems(guest_keys); i++) {
		observed = vmexit.vrs.vrs_gprs[VCPU_REGS_X11 + i];
		if (observed != guest_keys[i]) {
			warnx("guest key word %zu lost: got 0x%llx, wanted 0x%llx",
			    i, observed, guest_keys[i]);
			goto out_alarm;
		}
	}

	printf("preserved host and guest pointer-authentication keys across "
	    "vCPU exits\n");
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
