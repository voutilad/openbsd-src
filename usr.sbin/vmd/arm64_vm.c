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
#include <sys/ioctl.h>

#include <machine/armreg.h>

#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "vmd.h"
#include "vmm.h"
#include "arm64_vm.h"
#include "arm64_timer.h"
#include "gicv3.h"
#include "pl011.h"

#define EXCP_DATA_ABORT_L	0x24
#define HPFAR_FIPA_MASK		0xfffffffff0UL
#define ISS_DATA_SAS_SHIFT	22
#define ISS_DATA_SRT_SHIFT	16

/*
 * OpenBSD arm64 kernels are linked in the upper 128GB of the address space.
 * The native boot loader strips that virtual base and places the resulting
 * offsets in a 2MB-aligned, contiguous 64MB physical allocation.
 */
#define ARM64_KERNEL_VA_MASK	0x0000007fffffffffULL
#define ARM64_KERNEL_VA_BASE	0xffffff8000000000ULL
#define ARM64_KERNEL_ALIGN	(2UL * 1024 * 1024)
#define ARM64_KERNEL_WINDOW	(64UL * 1024 * 1024)

extern struct vmd	*env;
extern struct vmd_vm	*current_vm;
extern int		 con_fd;

static int	arm64_mmio_access(paddr_t, size_t, int, uint64_t *);
static int	elf_to_gpa(uint64_t, int, uint64_t *);
static int	load_payload_elf(gzFile, struct vmd_vm *,
		    struct vcpu_reg_state *);
static int	vcpu_exit_mmio(struct vm_run_params *);
static int	vcpu_exit_sysreg(struct vm_run_params *);

void
create_memory_map(struct vmd_vm *vm)
{
	struct vmop_create_params *vmc = &vm->vm_params;
	size_t memsize = vmc->vmc_memranges[0].vmr_size;

	/*
	 * The parser temporarily stores requested RAM in range zero.  Replace
	 * that input with the physical map vmm(4) will actually instantiate.
	 * Leaving the count at zero makes any rejected layout fail CREATE.
	 */
	vmc->vmc_nmemranges = 0;
	if (vmc->vmc_ncpus != 1 || memsize == 0 ||
	    memsize > VMM_MAX_VM_MEM_SIZE ||
	    (memsize & (ARM64_STAGE2_PAGE_SIZE - 1)) != 0)
		return;

	/*
	 * vmm(4) requires ascending, non-overlapping guest-physical ranges.
	 * VM_MEM_MMIO reserves IPA apertures but allocates no backing memory;
	 * accesses therefore exit to this process for GICv3 or PL011 emulation.
	 */
	vmc->vmc_memranges[0].vmr_gpa = ARM64_GICD_BASE;
	vmc->vmc_memranges[0].vmr_size = ARM64_GICD_SIZE;
	vmc->vmc_memranges[0].vmr_type = VM_MEM_MMIO;
	vmc->vmc_memranges[1].vmr_gpa = ARM64_GICR_BASE;
	vmc->vmc_memranges[1].vmr_size = ARM64_GICR_SIZE;
	vmc->vmc_memranges[1].vmr_type = VM_MEM_MMIO;
	vmc->vmc_memranges[2].vmr_gpa = ARM64_UART_BASE;
	vmc->vmc_memranges[2].vmr_size = ARM64_UART_SIZE;
	vmc->vmc_memranges[2].vmr_type = VM_MEM_MMIO;
	vmc->vmc_memranges[3].vmr_gpa = ARM64_RAM_BASE;
	vmc->vmc_memranges[3].vmr_size = memsize;
	vmc->vmc_memranges[3].vmr_type = VM_MEM_RAM;
	vmc->vmc_nmemranges = 4;
}

int
load_firmware(struct vmd_vm *vm, struct vcpu_reg_state *vrs)
{
	gzFile fp;
	int ret;

	/* Use vmd's usual zlib stream so plain and gzip payloads both work. */
	if ((fp = gzdopen(vm->vm_kernel, "r")) == NULL) {
		log_warnx("failed to open arm64 payload");
		return (-1);
	}
	ret = load_payload_elf(fp, vm, vrs);
	gzclose(fp);
	return (ret);
}

static int
elf_to_gpa(uint64_t addr, int kernel, uint64_t *gpa)
{
	uint64_t offset;

	if (!kernel) {
		*gpa = addr;
		return (0);
	}
	if ((addr & ~ARM64_KERNEL_VA_MASK) != ARM64_KERNEL_VA_BASE)
		return (-1);
	offset = addr & ARM64_KERNEL_VA_MASK;
	if (offset >= ARM64_KERNEL_WINDOW ||
	    ARM64_RAM_BASE > UINT64_MAX - offset)
		return (-1);
	*gpa = ARM64_RAM_BASE + offset;
	return (0);
}

/*
 * Load either a small fixed-address payload or an OpenBSD arm64 kernel.  A
 * kernel's ELF addresses are virtual addresses rooted at KERNBASE; matching
 * bootaa64, strip that base and load the offsets into a 2MB-aligned 64MB
 * physical window.  OpenBSD's locore discovers this VA-to-PA delta before it
 * enables its own stage-1 mappings.
 *
 * Symbols are not copied yet, so x0 names the end of the loaded PT_LOAD
 * image.  That is sufficient for early pmap bootstrap and leaves DDB symbol
 * loading as an independent extension.  x2 points at the FDT for both image
 * styles, which keeps the fixed-address regression payload useful.
 */
static int
load_payload_elf(gzFile fp, struct vmd_vm *vm, struct vcpu_reg_state *vrs)
{
	Elf64_Ehdr eh;
	Elf64_Phdr ph;
	struct vm_mem_range *ram;
	void *mem;
	uint64_t end, entry, fdt_gpa, image_end = 0, load_gpa, ram_end, stack;
	size_t fdt_size;
	int entry_ok = 0, kernel, loaded = 0, nread;
	unsigned int i;

	/* The loader may only write the RAM range created above. */
	ram = find_gpa_range(&vm->vm_params, ARM64_RAM_BASE, 1);
	if (ram == NULL || ram->vmr_type != VM_MEM_RAM)
		goto bad;
	if (ram->vmr_gpa > UINT64_MAX - ram->vmr_size ||
	    ram->vmr_size <= ARM64_FDT_SIZE + 16)
		goto bad;
	ram_end = ram->vmr_gpa + ram->vmr_size;
	fdt_gpa = ram_end - ARM64_FDT_SIZE;
	if (gzrewind(fp) == -1 || gzread(fp, &eh, sizeof(eh)) != sizeof(eh))
		goto bad;
	/* Reject anything except a native AArch64 executable. */
	if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
	    eh.e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh.e_ident[EI_DATA] != ELFDATA2LSB ||
	    eh.e_ident[EI_VERSION] != EV_CURRENT ||
	    eh.e_type != ET_EXEC || eh.e_machine != EM_AARCH64 ||
	    eh.e_version != EV_CURRENT || eh.e_ehsize != sizeof(eh) ||
	    eh.e_phentsize != sizeof(ph) || eh.e_phnum == 0 ||
	    eh.e_phnum > 64)
		goto bad;
	kernel = (eh.e_entry & ~ARM64_KERNEL_VA_MASK) ==
	    ARM64_KERNEL_VA_BASE;
	if (kernel && ((ARM64_RAM_BASE & (ARM64_KERNEL_ALIGN - 1)) != 0 ||
	    ram->vmr_size < ARM64_KERNEL_WINDOW))
		goto bad;
	if (elf_to_gpa(eh.e_entry, kernel, &entry) == -1)
		goto bad;

	for (i = 0; i < eh.e_phnum; i++) {
		/* Re-seek because loading one segment moves the gzip stream. */
		if (eh.e_phoff > INT64_MAX - i * sizeof(ph) ||
		    gzseek(fp, eh.e_phoff + i * sizeof(ph), SEEK_SET) == -1 ||
		    gzread(fp, &ph, sizeof(ph)) != sizeof(ph))
			goto bad;
		if (ph.p_type == PT_OPENBSD_RANDOMIZE) {
			if (ph.p_filesz > SIZE_MAX ||
			    elf_to_gpa(ph.p_paddr, kernel, &load_gpa) == -1 ||
			    load_gpa > UINT64_MAX - ph.p_filesz ||
			    load_gpa + ph.p_filesz > fdt_gpa)
				goto bad;
			mem = hvaddr_mem(load_gpa, ph.p_filesz);
			if (mem == NULL)
				goto bad;
			arc4random_buf(mem, ph.p_filesz);
			continue;
		}
		if (ph.p_type != PT_LOAD || ph.p_memsz == 0)
			continue;
		/*
		 * Fixed payloads use p_paddr as the IPA.  Kernel p_paddr values are
		 * translated to offsets in the physical load window first.
		 * hvaddr_mem() then rejects crossings into MMIO or outside RAM.
		 */
		if (ph.p_filesz > ph.p_memsz || ph.p_filesz > INT_MAX ||
		    ph.p_memsz > SIZE_MAX ||
		    elf_to_gpa(ph.p_paddr, kernel, &load_gpa) == -1 ||
		    load_gpa > UINT64_MAX - ph.p_memsz)
			goto bad;
		end = load_gpa + ph.p_memsz;
		if (kernel && end > ARM64_RAM_BASE + ARM64_KERNEL_WINDOW)
			goto bad;
		/* The top stage-2 page belongs to the firmware table, not ELF. */
		if (end > fdt_gpa)
			goto bad;
		mem = hvaddr_mem(load_gpa, ph.p_memsz);
		if (mem == NULL)
			goto bad;
		/* Zero the whole segment first so p_memsz - p_filesz is BSS. */
		memset(mem, 0, ph.p_memsz);
		if (ph.p_filesz != 0) {
			if (gzseek(fp, ph.p_offset, SEEK_SET) == -1)
				goto bad;
			nread = gzread(fp, mem, ph.p_filesz);
			if (nread < 0 || (uint64_t)nread != ph.p_filesz)
				goto bad;
		}
		/* Newly copied AArch64 instructions must be visible to the I-cache. */
		__builtin___clear_cache(mem, (char *)mem + ph.p_memsz);
		if ((ph.p_flags & PF_X) != 0 && entry >= load_gpa &&
		    entry < end)
			entry_ok = 1;
		if (end > image_end)
			image_end = end;
		loaded = 1;
	}
	/* Require an aligned entry point inside an executable load segment. */
	if (!loaded || !entry_ok || entry % sizeof(uint32_t) != 0)
		goto bad;
	mem = hvaddr_mem(fdt_gpa, ARM64_FDT_SIZE);
	if (mem == NULL || arm64_fdt_build(mem, ARM64_FDT_SIZE,
	    ram->vmr_size, &fdt_size) == -1)
		goto bad;

	/*
	 * Enter at EL1h with the MMU off.  Masking DAIF is important until a
	 * guest installs exception vectors and configures the GIC.  SCTLR_RES1 is
	 * the architecturally valid disabled-MMU value used by vmm(4)'s reset
	 * path.  x2 contains the guest-physical FDT address expected by the
	 * OpenBSD arm64 kernel ABI.  x0 is the physical image end used to set
	 * esym and delimit early kernel allocations; x1 is reserved and remains
	 * zero.  Fixed-address diagnostic payloads simply ignore x0.
	 */
	stack = fdt_gpa;
	memset(vrs, 0, sizeof(*vrs));
	vrs->vrs_pc = entry;
	vrs->vrs_sp = (stack - 16) & ~0xfUL;
	vrs->vrs_gprs[0] = image_end;
	vrs->vrs_gprs[2] = fdt_gpa;
	vrs->vrs_pstate = PSR_F | PSR_I | PSR_A | PSR_D | PSR_M_EL1h;
	vrs->vrs_sctlr_el1 = SCTLR_RES1;
	log_debug("%s: %s entry 0x%llx, end 0x%llx, stack 0x%llx, "
	    "FDT 0x%llx (%zu bytes)", __func__,
	    kernel ? "kernel" : "payload", vrs->vrs_pc, image_end,
	    vrs->vrs_sp, fdt_gpa, fdt_size);
	return (0);

bad:
	errno = ENOEXEC;
	log_warnx("invalid arm64 payload ELF");
	return (-1);
}

int
init_emulated_hw(struct vmd_vm *vm, int child_cdrom,
    int child_disks[][VM_MAX_BASE_PER_DISK], int *child_taps)
{
	(void)child_cdrom;
	(void)child_disks;
	(void)child_taps;

	/* Keep this first backend honest: it has only a GIC and polling PL011. */
	if (vm->vm_params.vmc_ndisks != 0 ||
	    vm->vm_params.vmc_nnics != 0 || vm->vm_cdrom != -1) {
		log_warnx("arm64 guests do not yet support storage or network "
		    "devices");
		return (EOPNOTSUPP);
	}
	gicv3_init(vm->vm_vmmid);
	arm64_timer_init(vm->vm_vmmid);
	pl011_init(con_fd, vm->vm_vmmid);
	return (0);
}

void
pause_vm_md(struct vmd_vm *vm)
{
	(void)vm;
	arm64_timer_pause();
	pl011_pause();
}

void
unpause_vm_md(struct vmd_vm *vm)
{
	(void)vm;
	arm64_timer_unpause();
	pl011_unpause();
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
		/* Subtract first, then compare, to avoid end-address overflow. */
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

	/* MMIO ranges have no userspace VA and must never be dereferenced. */
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

/*
 * Present an already-selected GIC interrupt to vmm(4)'s hardware CPU
 * interface.  The ioctl carries controller state, not an emulated guest
 * instruction: vmd retains all Distributor and Redistributor MMIO policy,
 * while IAR and EOIR execute directly against an EL2 List Register.
 */
int
arm64_vcpu_intr(uint32_t vm_id, uint32_t vcpu_id, uint16_t intid,
    uint8_t priority, int asserted)
{
	struct vm_intr_params vip;

	memset(&vip, 0, sizeof(vip));
	vip.vip_vm_id = vm_id;
	vip.vip_vcpu_id = vcpu_id;
	vip.vip_intr = intid;
	vip.vip_priority = priority;
	vip.vip_level = asserted ? VMM_INTR_LEVEL_HIGH : VMM_INTR_LEVEL_LOW;
	if (ioctl(env->vmd_vmm_fd, VMM_IOC_INTR, &vip) == -1)
		return (errno);
	return (0);
}

int
intr_pending(int vcpu_id)
{
	/* The arm64 GIC drives VMM_IOC_INTR directly, not the x86 injection ABI. */
	(void)vcpu_id;
	return (0);
}

void
intr_toggle_el(struct vmd_vm *vm, int irq, int val)
{
	/* There are no emulated interrupt lines to raise or lower yet. */
	(void)vm;
	(void)irq;
	(void)val;
}

int
intr_ack(int vcpu_id)
{
	/* The hardware virtual CPU interface completes ICC_IAR1_EL1. */
	(void)vm;
	return (-1);
}

void
vcpu_assert_vector(int fd, uint32_t vcpu_id, uint8_t vector)
{
	if (gicv3_set_irq(vm_id, vcpu_id, irq, 1) != 0)
		log_warnx("failed to assert GICv3 INTID %d", irq);
}

void
vcpu_assert_irq(int fd, uint32_t vcpu_id, int vector)
{
	if (gicv3_set_irq(vm_id, vcpu_id, irq, 0) != 0)
		log_warnx("failed to deassert GICv3 INTID %d", irq);
}

void
vcpu_deassert_irq(int fd, uint32_t vcpu_id, int vector)
{
	fatalx("%s: unimplemented", __func__);
}

int
vcpu_exit(struct vm_run_params *vrp)
{
	/*
	 * A stage-2 MMIO fault arrives as an exception exit.  HVC and trapped
	 * WFI/WFE are safe idle points: park the vCPU thread instead of
	 * immediately re-entering and consuming a host CPU.  An injected event
	 * or VM teardown can wake the generic vcpu_halt() wait.
	 */
	switch (vrp->vrp_exit_reason) {
	case VM_EXIT_EXCEPTION:
		switch (ESR_ELx_EXCEPTION(vrp->vrp_exit->vesr)) {
		case EXCP_DATA_ABORT_L:
			return (vcpu_exit_mmio(vrp));
		case EXCP_MSR:
			return (vcpu_exit_sysreg(vrp));
		default:
			log_warnx("unhandled arm64 exception: esr=0x%llx "
			    "pc=0x%llx", vrp->vrp_exit->vesr,
			    vrp->vrp_exit->vrs.vrs_pc);
			return (EFAULT);
		}
	case VM_EXIT_HVC:
		vcpu_halt(vrp->vrp_vcpu_id);
		return (0);
	case VM_EXIT_WFX:
		/*
		 * WFI/WFE is complete when vmm(4) reports it.  Leave the returned
		 * snapshot's resume PC after the instruction before parking.  The
		 * next VMM_IOC_RUN consumes that snapshot, so an IRQ taken while
		 * waking the vCPU cannot return to the same WFI and immediately sleep
		 * again after the interrupt source has been cleared.
		 */
		vrp->vrp_exit->vrs.vrs_pc += sizeof(uint32_t);
		vcpu_halt(vrp->vrp_vcpu_id);
		gicv3_wfi(vrp->vrp_vcpu_id);
		return (0);
	default:
		log_warnx("unexpected arm64 vcpu exit reason 0x%x",
		    vrp->vrp_exit_reason);
		return (EIO);
	}
}

static int
arm64_mmio_access(paddr_t gpa, size_t len, int write, uint64_t *data)
{
	uint32_t uart_data;
	int error;

	if ((gpa >= ARM64_GICD_BASE &&
	    gpa - ARM64_GICD_BASE < ARM64_GICD_SIZE) ||
	    (gpa >= ARM64_GICR_BASE &&
	    gpa - ARM64_GICR_BASE < ARM64_GICR_SIZE))
		return (gicv3_mmio(gpa, len, write, data));

	/* PL011 exposes 32-bit registers even though the MMIO layer is wider. */
	if (gpa >= ARM64_UART_BASE &&
	    gpa - ARM64_UART_BASE < ARM64_UART_SIZE) {
		uart_data = *data;
		error = pl011_mmio(gpa, len, write, &uart_data);
		if (!write)
			*data = uart_data;
		return (error);
	}
	log_warnx("unhandled arm64 MMIO access at 0x%lx", gpa);
	return (EFAULT);
}

static int
vcpu_exit_mmio(struct vm_run_params *vrp)
{
	struct vm_exit *exit = vrp->vrp_exit;
	struct vcpu_reg_state *vrs = &exit->vrs;
	paddr_t gpa;
	uint64_t esr = exit->vesr;
	uint64_t data, mask;
	uint32_t reg, sas;
	size_t len;
	int error, write;

	/*
	 * Only a Data Abort taken from a lower EL can describe the stage-2
	 * load/store we emulate.  ISV says the remaining ISS fields (size,
	 * target register, direction) are valid; without them instruction
	 * decoding would be required.
	 */
	if (ESR_ELx_EXCEPTION(esr) != EXCP_DATA_ABORT_L ||
	    (esr & ISS_DATA_ISV) == 0) {
		log_warnx("unhandled arm64 exception: esr=0x%llx "
		    "far=0x%llx hpfar=0x%llx pc=0x%llx", esr,
		    exit->vfar, exit->vhpfar, vrs->vrs_pc);
		return (EFAULT);
	}

	/*
	 * A GICv3 uses byte priority registers, 32-bit control registers, and
	 * 64-bit affinity-routing registers.  Accept unsigned accesses of all
	 * architectural sizes and require the syndrome's register-width bit to
	 * agree with the transfer.  Signed loads are not needed by either the
	 * PL011 or GIC register interfaces.
	 */
	sas = (esr & ISS_DATA_SAS_MASK) >> ISS_DATA_SAS_SHIFT;
	len = 1UL << sas;
	reg = (esr & ISS_DATA_SRT_MASK) >> ISS_DATA_SRT_SHIFT;
	write = (esr & ISS_DATA_WnR) != 0;
	if ((esr & ISS_DATA_SSE) != 0 ||
	    ((len == sizeof(uint64_t)) != ((esr & ISS_DATA_SF) != 0))) {
		log_warnx("unsupported arm64 MMIO access: esr=0x%llx", esr);
		return (EOPNOTSUPP);
	}

	/*
	 * HPFAR_EL2 supplies the faulting IPA above the low 12 bits; FAR_EL2
	 * supplies the page offset.  HPFAR also carries the upper offset bits
	 * needed by a 16KB stage-2 granule, so PAGE_MASK remains the native
	 * 4KB mask here.
	 */
	gpa = ((exit->vhpfar & HPFAR_FIPA_MASK) << 8) |
	    (exit->vfar & PAGE_MASK);
	data = 0;
	/* SRT==31 denotes WZR/XZR: writes are zero and loads are discarded. */
	mask = (len == sizeof(uint64_t)) ? UINT64_MAX : (1UL << (len * 8)) - 1;
	if (write && reg != 31)
		data = vrs->vrs_gprs[reg] & mask;
	error = arm64_mmio_access(gpa, len, write, &data);
	if (error)
		return (error);
	if (!write && reg != 31)
		vrs->vrs_gprs[reg] = data & mask;
	/* AArch64 instructions are fixed at four bytes; finish the access once. */
	vrs->vrs_pc += sizeof(uint32_t);
	return (0);
}

static int
vcpu_exit_sysreg(struct vm_run_params *vrp)
{
	struct vm_exit *exit = vrp->vrp_exit;
	struct vcpu_reg_state *vrs = &exit->vrs;
	uint64_t data = 0, esr = exit->vesr;
	u_int reg;
	int error, read;

	/*
	 * EXCP_MSR's ISS names the op fields and Rt without decoding guest
	 * memory.  DIR distinguishes MRS (read) from MSR (write).  vmm(4)
	 * leaves PC on the trapped instruction, so completion consists of
	 * supplying/consuming Rt, advancing four bytes, and writing the whole
	 * architectural snapshot back before the next VMM_IOC_RUN.
	 */
	if ((esr & ESR_ELx_IL) == 0)
		return (EFAULT);
	reg = ISS_MSR_Rt(esr);
	read = (esr & ISS_MSR_DIR) != 0;
	if (!read && reg != 31)
		data = vrs->vrs_gprs[reg];
	error = gicv3_icc(esr, !read, &data);
	if (error == ENOENT)
		error = arm64_timer_sysreg(esr, !read, &data);
	if (error != 0) {
		log_warnx("unhandled arm64 system register: esr=0x%llx "
		    "pc=0x%llx", esr, vrs->vrs_pc);
		return (error);
	}
	if (read && reg != 31)
		vrs->vrs_gprs[reg] = data;
	vrs->vrs_pc += sizeof(uint32_t);
	return (0);
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
