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
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int
create_vm(int vmm_fd, unsigned int result)
{
	struct vm_create_params create;
	struct vm_resetcpu_params reset;
	struct vm_sharemem_params share;
	uint32_t code[] = { 0xd2800000 | (result << 5), 0xd4000002 };
	char *mem;

	memset(&create, 0, sizeof(create));
	strlcpy(create.vcp_name, "api", sizeof(create.vcp_name));
	create.vcp_ncpus = 1;
	create.vcp_nmemranges = 1;
	create.vcp_memranges[0].vmr_size = 4 * PAGE_SIZE;
	if (ioctl(vmm_fd, VMM_IOC_CREATE, &create) == -1)
		err(1, "CREATE");
	mem = (char *)create.vcp_memranges[0].vmr_va;
	memcpy(mem, code, sizeof(code));
	__builtin___clear_cache(mem, mem + sizeof(code));

	/* SHAREMEM obtains mappings from the fd, not a caller-supplied layout. */
	memset(&share, 0, sizeof(share));
	if (ioctl(create.vcp_fd, VMM_IOC_SHAREMEM, &share) == -1)
		err(1, "SHAREMEM");
	((char *)share.vsp_va[0])[PAGE_SIZE] = result;
	if ((unsigned char)mem[PAGE_SIZE] != result)
		errx(1, "SHAREMEM did not alias the VM's RAM");

	memset(&reset, 0, sizeof(reset));
	reset.vrp_init_state.vrs_sp = 4 * PAGE_SIZE;
	reset.vrp_init_state.vrs_pstate = PSR_F | PSR_I | PSR_A | PSR_D |
	    PSR_M_EL1h;
	reset.vrp_init_state.vrs_sctlr_el1 = SCTLR_RES1;
	if (ioctl(create.vcp_fd, VMM_IOC_RESETCPU, &reset) == -1)
		err(1, "RESETCPU");
	return (create.vcp_fd);
}

static void
run_vm(int fd, unsigned int result)
{
	struct vm_run_params run;
	struct vm_exit vmexit;
	struct vm_rwregs_params regs;
	struct vm_irqcfg_params irqcfg;

	/* Exercise the arm64-specific ioctl through the pledged VM fd path. */
	memset(&irqcfg, 0, sizeof(irqcfg));
	irqcfg.viq_intr = VMM_ARM64_TIMER_INTID;
	if (ioctl(fd, VMM_IOC_IRQCFG, &irqcfg) == -1)
		err(1, "IRQCFG");
	irqcfg.viq_intr++;
	if (ioctl(fd, VMM_IOC_IRQCFG, &irqcfg) != -1 || errno != EINVAL)
		errx(1, "IRQCFG accepted an unsupported interrupt");

	memset(&run, 0, sizeof(run));
	memset(&vmexit, 0, sizeof(vmexit));
	run.vrp_exit = &vmexit;
	alarm(10);
	do {
		if (ioctl(fd, VMM_IOC_RUN, &run) == -1)
			err(1, "RUN");
	} while (run.vrp_exit_reason == VM_EXIT_NONE);
	alarm(0);
	if (run.vrp_exit_reason != VM_EXIT_HVC ||
	    vmexit.vrs.vrs_gprs[VCPU_REGS_X0] != result)
		errx(1, "VM fd returned the wrong guest state");
	memset(&regs, 0, sizeof(regs));
	regs.vrwp_mask = VM_RWREGS_ALL;
	if (ioctl(fd, VMM_IOC_READREGS, &regs) == -1)
		err(1, "READREGS");
	if (regs.vrwp_regs.vrs_gprs[VCPU_REGS_X0] != result)
		errx(1, "READREGS returned the wrong guest state");
	regs.vrwp_vcpu_id = 1;
	if (ioctl(fd, VMM_IOC_READREGS, &regs) != -1 || errno != ENOENT)
		errx(1, "READREGS accepted a nonexistent vCPU");
}

int
main(void)
{
	struct vm_run_params run;
	int vmm_fd, first, second, copy;

	if ((vmm_fd = open("/dev/vmm", O_RDWR)) == -1)
		err(1, "open /dev/vmm");
	memset(&run, 0, sizeof(run));
	if (ioctl(vmm_fd, VMM_IOC_RUN, &run) != -1 || errno != ENOTTY)
		errx(1, "device fd accepted a per-VM RUN");
	first = create_vm(vmm_fd, 0x42);
	second = create_vm(vmm_fd, 0x99);
	close(vmm_fd);
	if ((copy = dup(first)) == -1)
		err(1, "dup VM fd");
	close(first);
	if (pledge("stdio vmm", NULL) == -1)
		err(1, "pledge");
	/* Closing the creation fd and one duplicate must not kill either VM. */
	run_vm(copy, 0x42);
	close(copy);
	run_vm(second, 0x99);
	close(second);
	puts("VM descriptors preserve ownership, isolation and pledged ioctls");
	return (0);
}
