/*	$OpenBSD: arm64_vm.c,v 1.14 2026/09/19 17:21:52 dv Exp $	*/
/*
 * Copyright (c) 2024 Dave Voutila <dv@openbsd.org>
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

#include <errno.h>
#include <string.h>

#include "vmd.h"
#include "vmm.h"
#include "arm64_vm.h"

extern struct vmd_vm	*current_vm;

void
create_memory_map(struct vmd_vm *vm)
{
	struct vmop_create_params *vmc = &vm->vm_params;
	size_t memsize = vmc->vmc_memranges[0].vmr_size;

	vmc->vmc_nmemranges = 0;
	if (vmc->vmc_ncpus != 1 || memsize == 0 ||
	    memsize > VMM_MAX_VM_MEM_SIZE ||
	    (memsize & (ARM64_UART_SIZE - 1)) != 0)
		return;

	vmc->vmc_memranges[0].vmr_gpa = ARM64_RAM_BASE;
	vmc->vmc_memranges[0].vmr_size = memsize;
	vmc->vmc_memranges[0].vmr_type = VM_MEM_RAM;
	vmc->vmc_memranges[1].vmr_gpa = ARM64_UART_BASE;
	vmc->vmc_memranges[1].vmr_size = ARM64_UART_SIZE;
	vmc->vmc_memranges[1].vmr_type = VM_MEM_MMIO;
	vmc->vmc_nmemranges = 2;
}

int
load_firmware(struct vmd_vm *vm, struct vcpu_reg_state *vrs)
{
	fatalx("%s: unimplemented", __func__);
	/* NOTREACHED */
	return (-1);
}

int
init_emulated_hw(struct vmd_vm *vm, int child_cdrom,
    int child_disks[][VM_MAX_BASE_PER_DISK], int *child_taps)
{
	(void)child_cdrom;
	(void)child_disks;
	(void)child_taps;

	if (vm->vm_params.vmc_ndisks != 0 ||
	    vm->vm_params.vmc_nnics != 0 || vm->vm_cdrom != -1) {
		log_warnx("arm64 guests do not yet support storage or network "
		    "devices");
		return (EOPNOTSUPP);
	}
	return (0);
}

void
pause_vm_md(struct vmd_vm *vm)
{
	(void)vm;
}

void
unpause_vm_md(struct vmd_vm *vm)
{
	(void)vm;
}

struct vm_mem_range *
find_gpa_range(struct vmop_create_params *vmc, paddr_t gpa, size_t len)
{
	struct vm_mem_range *vmr;
	size_t i, off;

	for (i = 0; i < vmc->vmc_nmemranges; i++) {
		vmr = &vmc->vmc_memranges[i];
		if (gpa < vmr->vmr_gpa)
			continue;
		off = gpa - vmr->vmr_gpa;
		if (off <= vmr->vmr_size && len <= vmr->vmr_size - off)
			return (vmr);
	}
	return (NULL);
}

void *
hvaddr_mem(paddr_t gpa, size_t len)
{
	struct vm_mem_range *vmr;
	size_t off;

	vmr = find_gpa_range(&current_vm->vm_params, gpa, len);
	if (vmr == NULL || vmr->vmr_type != VM_MEM_RAM) {
		errno = EFAULT;
		return (NULL);
	}
	off = gpa - vmr->vmr_gpa;
	return ((char *)vmr->vmr_va + off);
}

int
write_mem(paddr_t dst, const void *buf, size_t len)
{
	void *mem;

	mem = hvaddr_mem(dst, len);
	if (mem == NULL)
		return (EINVAL);
	if (buf == NULL)
		memset(mem, 0, len);
	else
		memcpy(mem, buf, len);
	return (0);
}

int
read_mem(paddr_t src, void *buf, size_t len)
{
	void *mem;

	mem = hvaddr_mem(src, len);
	if (mem == NULL)
		return (EINVAL);
	memcpy(buf, mem, len);
	return (0);
}

int
intr_pending(int vcpu_id)
{
	(void)vm;
	return (0);
}

void
intr_toggle_el(struct vmd_vm *vm, int irq, int val)
{
	(void)vm;
	(void)irq;
	(void)val;
}

int
intr_ack(int vcpu_id)
{
	(void)vm;
	return (-1);
}

void
vcpu_assert_vector(int fd, uint32_t vcpu_id, uint8_t vector)
{
	(void)vm_id;
	(void)vcpu_id;
	(void)irq;
}

void
vcpu_assert_irq(int fd, uint32_t vcpu_id, int vector)
{
	(void)vm_id;
	(void)vcpu_id;
	(void)irq;
}

void
vcpu_deassert_irq(int fd, uint32_t vcpu_id, int vector)
{
	fatalx("%s: unimplemented", __func__);
}

int
vcpu_exit(struct vm_run_params *vrp)
{
	fatalx("%s: unimplemented", __func__);
	/* NOTREACHED */
	return (-1);
}

uint8_t
vcpu_exit_pci(struct vm_run_params *vrp)
{
	(void)vrp;
	return (0xff);
}

void
set_return_data(struct vm_exit *vei, uint32_t data)
{
	fatalx("%s: unimplemented", __func__);
	/* NOTREACHED */
	return;
}

void
get_input_data(struct vm_exit *vei, uint32_t *data)
{
	fatalx("%s: unimplemented", __func__);
	/* NOTREACHED */
	return;
}

int
sev_init(struct vmd_vm *vm)
{
	return (vm->vm_params.vmc_sev ? EOPNOTSUPP : 0);
}

int
sev_shutdown(struct vmd_vm *vm)
{
	(void)vm;
	return (0);
}

int
sev_activate(struct vmd_vm *vm, int vcpu_id)
{
	(void)vcpu_id;
	return (vm->vm_params.vmc_sev ? EOPNOTSUPP : 0);
}

int
sev_encrypt_memory(struct vmd_vm *vm)
{
	return (vm->vm_params.vmc_sev ? EOPNOTSUPP : 0);
}

int
sev_encrypt_state(struct vmd_vm *vm, int vcpu_id)
{
	(void)vcpu_id;
	return (vm->vm_params.vmc_sev ? EOPNOTSUPP : 0);
}

int
sev_launch_finalize(struct vmd_vm *vm)
{
	return (vm->vm_params.vmc_sev ? EOPNOTSUPP : 0);
}

void
psp_setup(void)
{
}
