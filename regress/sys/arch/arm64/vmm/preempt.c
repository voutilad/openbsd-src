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

/*
 * Seed registers which are not exercised by the stage-2 fault regression,
 * then spin forever.  A physical host timer interrupt must take the CPU to
 * EL2 despite the guest PSTATE.I bit set below.  After that exit, the kernel
 * must report these values exactly as the guest left them.
 *
 * 0x1313 uses only architecturally writable SPSR_EL1 bits; reserved bits in
 * a more visually obvious pattern would be cleared by the CPU.
 */
static const uint32_t guest_code[] = {
	0xd2822221,	/* mov x1, #0x1111 */
	0xd5184101,	/* msr sp_el0, x1 */
	0xd2844441,	/* mov x1, #0x2222 */
	0xd5184021,	/* msr elr_el1, x1 */
	0xd2826261,	/* mov x1, #0x1313 */
	0xd5184001,	/* msr spsr_el1, x1 */
	0xd2888881,	/* mov x1, #0x4444 */
	0xd5185201,	/* msr esr_el1, x1 */
	0xd28aaaa1,	/* mov x1, #0x5555 */
	0xd5186001,	/* msr far_el1, x1 */
	0xd28cccc1,	/* mov x1, #0x6666 */
	0xd51bd041,	/* msr tpidr_el0, x1 */
	0xd28eeee1,	/* mov x1, #0x7777 */
	0xd51bd061,	/* msr tpidrro_el0, x1 */
	0x14000000,	/* b . */
};

static volatile sig_atomic_t alarm_fired;

static void
alarm_handler(int sig)
{
	(void)sig;
	alarm_fired = 1;
}

static int
check_register(const char *name, uint64_t value, uint64_t expected)
{
	if (value == expected)
		return (0);
	warnx("%s: got 0x%llx, expected 0x%llx", name, value, expected);
	return (1);
}

int
main(void)
{
	struct vm_create_params create;
	struct vm_exit exit;
	struct vm_resetcpu_params reset;
	struct vm_run_params run;
	struct vm_sharemem_params share;
	struct vm_terminate_params term;
	struct sigaction sa;
	int fd, rv, ret = 1, yields = 0;

	fd = open(VMM_NODE, O_RDWR);
	if (fd == -1)
		err(1, "open %s", VMM_NODE);

	memset(&create, 0, sizeof(create));
	strlcpy(create.vcp_name, "preempt", sizeof(create.vcp_name));
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
	memcpy((void *)create.vcp_memranges[0].vmr_va, guest_code,
	    sizeof(guest_code));
	/* Guest instruction fetch is not coherent with these data stores. */
	__builtin___clear_cache((char *)create.vcp_memranges[0].vmr_va,
	    (char *)create.vcp_memranges[0].vmr_va + sizeof(guest_code));

	memset(&reset, 0, sizeof(reset));
	reset.vrp_vm_id = create.vcp_id;
	reset.vrp_init_state.vrs_sp = PAGE_SIZE;
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

	memset(&exit, 0, sizeof(exit));
	memset(&run, 0, sizeof(run));
	run.vrp_vm_id = create.vcp_id;
	run.vrp_exit = &exit;
	/*
	 * VM_EXIT_NONE is the arm64 API's host-interrupt yield.  Retry it until
	 * SIGALRM proves the process can run while the guest remains busy.
	 */
	alarm(1);
	do {
		rv = ioctl(fd, VMM_IOC_RUN, &run);
		if (rv == 0 && run.vrp_exit_reason == VM_EXIT_NONE)
			yields++;
	} while (rv == 0 && !alarm_fired &&
	    run.vrp_exit_reason == VM_EXIT_NONE);
	alarm(0);
	if (rv == -1 && errno != EINTR) {
		warn("VMM_IOC_RUN");
		goto out;
	}
	if (!alarm_fired) {
		warnx("busy guest returned without alarm");
		goto out;
	}
	if (yields == 0) {
		warnx("busy guest did not yield");
		goto out;
	}
	/* Every asynchronous exit must be a complete architectural save. */
	if (check_register("sp_el0", exit.vrs.vrs_sp_el0, 0x1111) ||
	    check_register("elr_el1", exit.vrs.vrs_elr_el1, 0x2222) ||
	    check_register("spsr_el1", exit.vrs.vrs_spsr_el1, 0x1313) ||
	    check_register("esr_el1", exit.vrs.vrs_esr_el1, 0x4444) ||
	    check_register("far_el1", exit.vrs.vrs_far_el1, 0x5555) ||
	    check_register("tpidr_el0", exit.vrs.vrs_tpidr_el0, 0x6666) ||
	    check_register("tpidrro_el0", exit.vrs.vrs_tpidrro_el0, 0x7777))
		goto out;

	printf("busy guest yielded %d times and preserved EL1 context\n",
	    yields);
	ret = 0;

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
