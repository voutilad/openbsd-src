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
#define GUEST_MEM_SIZE	(8 * PAGE_SIZE)

#define BOOT_PA		0x0000UL
#define VECTOR1_PA	0x1000UL
#define VECTOR2_PA	0x2000UL
#define TARGET_PA	0x3000UL
#define L1_PA		0x4000UL
#define L2_PA		0x5000UL
#define L3_PA		0x6000UL
#define TARGET_VA	0x100000UL

#define VECTOR_LOWER_SYNC	0x400UL
#define PTE_VALID		(1ULL << 0)
#define PTE_TABLE		(1ULL << 1)
#define PTE_AP_USER		(1ULL << 6)
#define PTE_SH_INNER		(3ULL << 8)
#define PTE_AF			(1ULL << 10)
#define PTE_PAGE_KERNEL		(PTE_VALID | PTE_TABLE | PTE_SH_INNER | PTE_AF)
#define PTE_PAGE_USER		(PTE_PAGE_KERNEL | PTE_AP_USER)

/* 39-bit TTBR0, 4KB granule, WBWA inner-shareable, and no TTBR1 walk. */
#define TEST_TCR	(25ULL | (1ULL << 8) | (1ULL << 10) | \
			 (3ULL << 12) | (1ULL << 23) | (5ULL << 32))

extern char tlbi_boot_start[], tlbi_boot_end[];
extern char tlbi_fault_start[], tlbi_fault_end[];
extern char tlbi_target_start[], tlbi_target_end[];
extern char tlbi_success_start[], tlbi_success_end[];

static volatile sig_atomic_t alarm_fired;

static void
alarm_handler(int sig)
{
	(void)sig;
	alarm_fired = 1;
}

static void
copy_code(char *mem, size_t offset, char *start, char *end)
{
	size_t len = end - start;

	memcpy(mem + offset, start, len);
	__builtin___clear_cache(mem + offset, mem + offset + len);
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
	uint64_t *l1, *l2, *l3;
	uint64_t target_pte;
	char *mem;
	int error, fd, i, ret = 1;

	fd = open(VMM_NODE, O_RDWR);
	if (fd == -1)
		err(1, "open %s", VMM_NODE);

	memset(&create, 0, sizeof(create));
	strlcpy(create.vcp_name, "tlbi", sizeof(create.vcp_name));
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
	memset(mem, 0, GUEST_MEM_SIZE);

	copy_code(mem, BOOT_PA, tlbi_boot_start, tlbi_boot_end);
	copy_code(mem, VECTOR1_PA + VECTOR_LOWER_SYNC,
	    tlbi_fault_start, tlbi_fault_end);
	copy_code(mem, VECTOR2_PA + VECTOR_LOWER_SYNC,
	    tlbi_success_start, tlbi_success_end);
	copy_code(mem, TARGET_PA, tlbi_target_start, tlbi_target_end);

	l1 = (uint64_t *)(mem + L1_PA);
	l2 = (uint64_t *)(mem + L2_PA);
	l3 = (uint64_t *)(mem + L3_PA);
	l1[0] = L2_PA | PTE_VALID | PTE_TABLE;
	l2[0] = L3_PA | PTE_VALID | PTE_TABLE;
	/* Identity-map bootstrap, both vector pages, and the page tables. */
	for (i = 0; i <= 6; i++)
		l3[i] = (uint64_t)i * PAGE_SIZE | PTE_PAGE_KERNEL;
	target_pte = TARGET_PA | PTE_PAGE_USER;
	/* l3[TARGET_VA >> 12] deliberately remains invalid initially. */
	__builtin___clear_cache(mem, mem + GUEST_MEM_SIZE);

	memset(&reset, 0, sizeof(reset));
	reset.vrp_vm_id = create.vcp_id;
	reset.vrp_init_state.vrs_pc = BOOT_PA;
	reset.vrp_init_state.vrs_pstate = PSR_F | PSR_I | PSR_A | PSR_D |
	    PSR_M_EL1h;
	reset.vrp_init_state.vrs_elr_el1 = TARGET_VA;
	reset.vrp_init_state.vrs_spsr_el1 = PSR_F | PSR_I | PSR_A | PSR_D |
	    PSR_M_EL0t;
	reset.vrp_init_state.vrs_sctlr_el1 = SCTLR_RES1 | SCTLR_M |
	    SCTLR_C | SCTLR_I;
	reset.vrp_init_state.vrs_tcr_el1 = TEST_TCR;
	reset.vrp_init_state.vrs_ttbr0_el1 = L1_PA;
	reset.vrp_init_state.vrs_mair_el1 = 0xff;
	reset.vrp_init_state.vrs_vbar_el1 = VECTOR1_PA;
	reset.vrp_init_state.vrs_gprs[VCPU_REGS_X0] = L3_PA +
	    ((TARGET_VA >> 12) & 0x1ff) * sizeof(uint64_t);
	reset.vrp_init_state.vrs_gprs[VCPU_REGS_X1] = target_pte;
	reset.vrp_init_state.vrs_gprs[VCPU_REGS_X2] = TARGET_VA >> 12;
	reset.vrp_init_state.vrs_gprs[VCPU_REGS_X3] = VECTOR2_PA;
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
	alarm(10);
	error = run_to_exit(fd, &run);
	alarm(0);
	if (error != 0) {
		warnc(error, "guest TLBI did not make its new mapping visible "
		    "(handler count 0x%llx, pc 0x%llx, elr 0x%llx)",
		    vmexit.vrs.vrs_gprs[VCPU_REGS_X4], vmexit.vrs.vrs_pc,
		    vmexit.vrs.vrs_elr_el1);
		goto out;
	}
	if (run.vrp_exit_reason != VM_EXIT_HVC ||
	    vmexit.vrs.vrs_gprs[VCPU_REGS_X0] != 0x42) {
		warnx("unexpected exit 0x%x: x0 0x%llx pc 0x%llx esr 0x%llx",
		    run.vrp_exit_reason, vmexit.vrs.vrs_gprs[VCPU_REGS_X0],
		    vmexit.vrs.vrs_pc, vmexit.vesr);
		goto out;
	}
	if (vmexit.vrs.vrs_gprs[VCPU_REGS_X4] != 1) {
		warnx("mapping required 0x%llx instruction-abort retries",
		    vmexit.vrs.vrs_gprs[VCPU_REGS_X4]);
		goto out;
	}

	printf("guest stage-1 TLBI exposed a newly executable EL0 page\n");
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
