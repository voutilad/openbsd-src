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
#include <sys/systm.h>
#include <sys/atomic.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/pledge.h>
#include <sys/proc.h>

#include <uvm/uvm.h>
#include <uvm/uvm_extern.h>
#include <uvm/uvm_page.h>

#include <machine/armreg.h>
#include <machine/cpu.h>
#include <machine/hypervisor.h>
#include <machine/intr.h>
#include <machine/pmap.h>
#include <machine/vmmvar.h>

#include <dev/vmm/vmm.h>

#define EXCP_WFX		0x01
#define EXCP_HVC64		0x16

#define VTCR_T0SZ_39		(64 - 39)
#define VTCR_SL0_L1		(1UL << 6)
#define VTCR_IRGN0_WBWA		(1UL << 8)
#define VTCR_ORGN0_WBWA		(1UL << 10)
#define VTCR_SH0_INNER		(3UL << 12)
#define VTCR_PS_40BIT		(2UL << 16)
#define VTCR_RES1		(1UL << 31)
#define VTCR_STAGE2_39		(VTCR_RES1 | VTCR_PS_40BIT | \
	VTCR_SH0_INNER | VTCR_ORGN0_WBWA | VTCR_IRGN0_WBWA | \
	VTCR_SL0_L1 | VTCR_T0SZ_39)

#define HPFAR_FIPA_MASK		0xfffffffff0UL
#define VTTBR_VMID_SHIFT	48
#define VMM_S2_PAGE_SIZE	(4 * PAGE_SIZE)
#define VMM_S2_PAGE_MASK	(VMM_S2_PAGE_SIZE - 1)

int	arm64_vmm_enter_nvhe(paddr_t);
int	arm64_vmm_enter_vhe(vaddr_t);

static int	arm64_vmm_fault_page(struct vcpu *, paddr_t);
static int	arm64_vmm_alloc_memory(struct vm *);
static int	arm64_vmm_memtype(struct vm *, paddr_t);
static vaddr_t	arm64_vmm_translate_gpa(struct vm *, paddr_t);
static int	arm64_vmm_intr_pending(struct vm_intr_params *);
static void	arm64_vmm_load_run(struct vcpu *);
static void	arm64_vmm_save_run(struct vcpu *);

int
vmm_enabled(void)
{
	uint64_t pfr0;

	if (!arm64_has_el2)
		return (0);
	pfr0 = READ_SPECIALREG(id_aa64pfr0_el1);
	return (ID_AA64PFR0_EL2(pfr0) != ID_AA64PFR0_EL2_NONE);
}

int
vmm_probe_machdep(struct device *parent, void *match, void *aux)
{
	return (vmm_enabled());
}

void
vmm_attach_machdep(struct device *parent, struct device *self, void *aux)
{
	struct vmm_softc *sc = (struct vmm_softc *)self;

	sc->sc_md.nr_cpus = 1;
	sc->mode = VMM_MODE_STAGE2;
	sc->max_vpid = 0;
	rw_init(&sc->vpid_lock, "vpid");
	printf(": EL2/%s stage-2", arm64_has_el2 == 2 ? "VHE" : "nVHE");
}

void
vmm_activate_machdep(struct device *self, int act)
{
}

int
vmm_start(void)
{
	return (0);
}

int
vmm_stop(void)
{
	return (0);
}

int
vmmioctl_machdep(dev_t dev, u_long cmd, caddr_t data, int flag,
    struct proc *p)
{
	switch (cmd) {
	case VMM_IOC_INTR:
		return (arm64_vmm_intr_pending(
		    (struct vm_intr_params *)data));
	default:
		return (ENOTTY);
	}
}

static int
arm64_vmm_intr_pending(struct vm_intr_params *vip)
{
	struct vm *vm;
	struct vcpu *vcpu;
#ifdef MULTIPROCESSOR
	struct cpu_info *ci;
#endif
	int error, ret = 0;

	error = vm_find(vip->vip_vm_id, &vm);
	if (error != 0)
		return (error);
	vcpu = vm_find_vcpu(vm, vip->vip_vcpu_id);
	if (vcpu == NULL) {
		ret = ENOENT;
		goto out;
	}

	/*
	 * HCR_EL2 has a virtual IRQ input but no interrupt identity.  Until a
	 * userland GIC exists, preserve the VMM_IOC_INTR interface while treating
	 * vip_intr as a line level: zero lowers VI and any non-zero value raises
	 * it.  In particular, vip_intr is not an arm64 INTID here.
	 *
	 * vm_run() holds vc_lock across guest execution, so this ioctl cannot use
	 * that lock: it must be able to interrupt a running vCPU.  The naturally
	 * aligned value and READ_ONCE/WRITE_ONCE pair make the concurrent access
	 * indivisible and prevent the compiler from caching it.
	 */
	WRITE_ONCE(vcpu->vc_intr, vip->vip_intr);
#ifdef MULTIPROCESSOR
	/*
	 * A vCPU executing on another CPU will not rebuild HCR_EL2 until it
	 * returns from the guest.  The no-op IPI supplies that exit without
	 * attaching any interrupt-controller semantics to the kick.  If the vCPU
	 * stops or migrates while vc_curcpu is sampled, the kick may be redundant;
	 * the pending line remains recorded and is seen on its next entry.
	 */
	ci = READ_ONCE(vcpu->vc_curcpu);
	if (ci != NULL)
		arm_send_ipi(ci, ARM_IPI_NOP);
#endif
out:
	refcnt_rele_wake(&vm->vm_refcnt);
	return (ret);
}

int
pledge_ioctl_vmm_machdep(struct proc *p, long com)
{
	if (com == VMM_IOC_INTR)
		return (0);
	return (EPERM);
}

int
vm_impl_init(struct vm *vm, struct proc *p)
{
	int error;

	error = arm64_vmm_alloc_memory(vm);
	if (error)
		return (error);
	pmap_convert(vm->vm_pmap, PMAP_TYPE_STAGE2);
	return (0);
}

void
vm_impl_deinit(struct vm *vm)
{
}

/*
 * Allocate guest RAM in aligned 16KB runs.  A 16KB stage-2 implementation
 * can only compose four 4KB mappings when their input and output offsets
 * agree.  Keeping each run physically contiguous and aligned satisfies that
 * requirement while retaining the kernel's native 4KB page tables.
 */
static int
arm64_vmm_alloc_memory(struct vm *vm)
{
	struct pglist plist;
	struct uvm_object *uao;
	struct vm_mem_range *vmr;
	struct vm_page *pg;
	voff_t off;
	int error, i, n;

	for (i = 0; i < vm->vm_nmemranges; i++) {
		vmr = &vm->vm_memranges[i];
		if (vmr->vmr_type == VM_MEM_MMIO)
			continue;
		if ((vmr->vmr_gpa & VMM_S2_PAGE_MASK) != 0 ||
		    (vmr->vmr_size & VMM_S2_PAGE_MASK) != 0)
			return (EINVAL);

		uao = vm->vm_memory_slot[i];
		KASSERT(uao != NULL);
		for (off = 0; off < vmr->vmr_size; off += VMM_S2_PAGE_SIZE) {
			TAILQ_INIT(&plist);
			error = uvm_pglistalloc(VMM_S2_PAGE_SIZE, 0,
			    (paddr_t)-1, VMM_S2_PAGE_SIZE, 0, &plist, 1,
			    UVM_PLA_WAITOK | UVM_PLA_ZERO);
			if (error)
				return (error);

			rw_enter(uao->vmobjlock, RW_WRITE);
			n = 0;
			while ((pg = TAILQ_FIRST(&plist)) != NULL) {
				TAILQ_REMOVE(&plist, pg, pageq);
				uvm_pagealloc_pg(pg, uao, off + ptoa(n++), NULL);
				atomic_setbits_int(&pg->pg_flags,
				    PG_CLEAN | PQ_AOBJ);
				atomic_clearbits_int(&pg->pg_flags,
				    PG_FAKE | PG_BUSY);
				UVM_PAGE_OWN(pg, NULL);
				uvm_pageactivate(pg);
			}
			rw_exit(uao->vmobjlock);
			KASSERT(n == VMM_S2_PAGE_SIZE / PAGE_SIZE);
		}
	}
	return (0);
}

int
vcpu_init(struct vcpu *vcpu, struct vm_create_params *vcp)
{
	struct arm64_vmm_run *run;

	/*
	 * nVHE enters EL2 with stage 1 disabled, so its vector code addresses
	 * this page by physical address.  VHE uses the same page through its
	 * kernel virtual address because the host EL2 stage 1 remains enabled.
	 */
	vcpu->vc_control_va = (vaddr_t)km_alloc(PAGE_SIZE, &kv_page, &kp_zero,
	    &kd_waitok);
	if (vcpu->vc_control_va == 0)
		return (ENOMEM);
	if (!pmap_extract(pmap_kernel(), vcpu->vc_control_va,
	    &vcpu->vc_control_pa)) {
		km_free((void *)vcpu->vc_control_va, PAGE_SIZE, &kv_page, &kp_zero);
		vcpu->vc_control_va = 0;
		return (ENOMEM);
	}

	vcpu->vc_virt_mode = VMM_MODE_STAGE2;
	vcpu->vc_state = VCPU_STATE_STOPPED;
	rw_init(&vcpu->vc_lock, "vcpu");
	vcpu->vc_regs.vrs_pstate = PSR_F | PSR_I | PSR_A | PSR_D |
	    PSR_M_EL1h;
	vcpu->vc_regs.vrs_sctlr_el1 = SCTLR_RES1;
	run = (struct arm64_vmm_run *)vcpu->vc_control_va;
	/* A recycled VMID must not inherit translations from an older VM. */
	run->avr_flush_tlb = 1;
	return (0);
}

void
vcpu_deinit(struct vcpu *vcpu)
{
	if (vcpu->vc_control_va != 0) {
		km_free((void *)vcpu->vc_control_va, PAGE_SIZE, &kv_page,
		    &kp_zero);
		vcpu->vc_control_va = 0;
	}
}

int
vcpu_reset_regs(struct vcpu *vcpu, struct vcpu_reg_state *vrs)
{
	if ((vrs->vrs_pc & (INSN_SIZE - 1)) != 0)
		return (EINVAL);
	if ((vrs->vrs_pstate & PSR_M_MASK) != PSR_M_EL1h)
		return (EINVAL);

	memcpy(&vcpu->vc_regs, vrs, sizeof(vcpu->vc_regs));
	if (vcpu->vc_regs.vrs_sctlr_el1 == 0)
		vcpu->vc_regs.vrs_sctlr_el1 = SCTLR_RES1;
	return (0);
}

int
vm_rwregs(struct vm_rwregs_params *vrwp, int dir)
{
	struct vm *vm;
	struct vcpu *vcpu;
	int error, ret = 0;

	error = vm_find(vrwp->vrwp_vm_id, &vm);
	if (error)
		return (error);
	vcpu = vm_find_vcpu(vm, vrwp->vrwp_vcpu_id);
	if (vcpu == NULL) {
		ret = ENOENT;
		goto out;
	}
	if ((vrwp->vrwp_mask & ~VM_RWREGS_ALL) != 0) {
		ret = EINVAL;
		goto out;
	}

	rw_enter_write(&vcpu->vc_lock);
	if (vcpu->vc_state != VCPU_STATE_STOPPED)
		ret = EBUSY;
	else if (dir == 0)
		memcpy(&vrwp->vrwp_regs, &vcpu->vc_regs,
		    sizeof(vcpu->vc_regs));
	else
		memcpy(&vcpu->vc_regs, &vrwp->vrwp_regs,
		    sizeof(vcpu->vc_regs));
	rw_exit_write(&vcpu->vc_lock);
out:
	refcnt_rele_wake(&vm->vm_refcnt);
	return (ret);
}

int
vm_rwvmparams(struct vm_rwvmparams_params *vpp, int dir)
{
	return (EOPNOTSUPP);
}

static int
arm64_vmm_memtype(struct vm *vm, paddr_t gpa)
{
	struct vm_mem_range *vmr;
	int i;

	for (i = 0; i < vm->vm_nmemranges; i++) {
		vmr = &vm->vm_memranges[i];
		if (gpa < vmr->vmr_gpa)
			break;
		if (gpa < vmr->vmr_gpa + vmr->vmr_size) {
			if (vmr->vmr_type == VM_MEM_MMIO)
				return (VMM_MEM_TYPE_MMIO);
			return (VMM_MEM_TYPE_REGULAR);
		}
	}
	return (VMM_MEM_TYPE_UNKNOWN);
}

static vaddr_t
arm64_vmm_translate_gpa(struct vm *vm, paddr_t gpa)
{
	struct vm_mem_range *vmr;
	int i;

	for (i = 0; i < vm->vm_nmemranges; i++) {
		vmr = &vm->vm_memranges[i];
		if (gpa >= vmr->vmr_gpa &&
		    gpa < vmr->vmr_gpa + vmr->vmr_size)
			return (vmr->vmr_va + (gpa - vmr->vmr_gpa));
	}
	return (0);
}

static int
arm64_vmm_fault_page(struct vcpu *vcpu, paddr_t gpa)
{
	struct proc *p = curproc;
	struct arm64_vmm_run *run;
	paddr_t hpa, mapped, ipa = trunc_page(gpa);
	paddr_t ipa_base = ipa & ~VMM_S2_PAGE_MASK;
	vaddr_t hva;
	int error, i;

	if (pmap_extract(vcpu->vc_parent->vm_pmap, ipa, &mapped))
		return (0);

	/*
	 * The hardware stage-2 granule is 16KB even though OpenBSD pages are
	 * 4KB.  Fault and install the complete aligned group so one hardware
	 * descriptor always covers four contiguous host pages.
	 */
	if (arm64_vmm_memtype(vcpu->vc_parent, ipa_base) !=
	    VMM_MEM_TYPE_REGULAR)
		return (EFAULT);
	if (arm64_vmm_memtype(vcpu->vc_parent,
	    ipa_base + VMM_S2_PAGE_SIZE - PAGE_SIZE) != VMM_MEM_TYPE_REGULAR)
		return (EFAULT);
	hva = arm64_vmm_translate_gpa(vcpu->vc_parent, ipa_base);
	if (hva == 0)
		return (EFAULT);

	error = uvm_fault_wire(&p->p_vmspace->vm_map, hva,
	    hva + VMM_S2_PAGE_SIZE, PROT_READ | PROT_WRITE);
	if (error)
		return (error);
	for (i = 0; i < VMM_S2_PAGE_SIZE / PAGE_SIZE; i++) {
		if (!pmap_extract(p->p_vmspace->vm_map.pmap,
		    hva + i * PAGE_SIZE, &hpa))
			return (EFAULT);
		error = pmap_enter(vcpu->vc_parent->vm_pmap,
		    ipa_base + i * PAGE_SIZE, hpa,
		    PROT_READ | PROT_WRITE | PROT_EXEC,
		    PROT_READ | PROT_WRITE | PROT_EXEC);
		if (error)
			return (error);
	}
	run = (struct arm64_vmm_run *)vcpu->vc_control_va;
	/* A cached translation fault may survive until the next TLBI. */
	run->avr_flush_tlb = 1;
	return (error);
}

static void
arm64_vmm_load_run(struct vcpu *vcpu)
{
	struct arm64_vmm_run *run = (struct arm64_vmm_run *)vcpu->vc_control_va;
	struct vcpu_reg_state *vrs = &vcpu->vc_regs;

	memcpy(run->avr_gprs, vrs->vrs_gprs, sizeof(run->avr_gprs));
	run->avr_sp = vrs->vrs_sp;
	run->avr_sp_el0 = vrs->vrs_sp_el0;
	run->avr_pc = vrs->vrs_pc;
	run->avr_pstate = vrs->vrs_pstate;
	run->avr_elr_el1 = vrs->vrs_elr_el1;
	run->avr_spsr_el1 = vrs->vrs_spsr_el1;
	run->avr_esr_el1 = vrs->vrs_esr_el1;
	run->avr_far_el1 = vrs->vrs_far_el1;
	run->avr_sctlr_el1 = vrs->vrs_sctlr_el1;
	run->avr_tcr_el1 = vrs->vrs_tcr_el1;
	run->avr_ttbr0_el1 = vrs->vrs_ttbr0_el1;
	run->avr_ttbr1_el1 = vrs->vrs_ttbr1_el1;
	run->avr_mair_el1 = vrs->vrs_mair_el1;
	run->avr_vbar_el1 = vrs->vrs_vbar_el1;
	run->avr_contextidr_el1 = vrs->vrs_contextidr_el1;
	run->avr_cpacr_el1 = vrs->vrs_cpacr_el1;
	run->avr_tpidr_el0 = vrs->vrs_tpidr_el0;
	run->avr_tpidrro_el0 = vrs->vrs_tpidrro_el0;
	run->avr_tpidr_el1 = vrs->vrs_tpidr_el1;
	run->avr_vttbr_el2 = vcpu->vc_parent->vm_pmap->pm_pt0pa |
	    ((uint64_t)(vcpu->vc_parent->vm_id & 0xff) << VTTBR_VMID_SHIFT);
	run->avr_vtcr_el2 = VTCR_STAGE2_39;
	/*
	 * VM enables stage 2 and RW selects an AArch64 EL1 guest.  TWI/TWE
	 * make WFI/WFE observable to the API.  API/APK leave pointer
	 * authentication available.  IMO/FMO/AMO route physical asynchronous
	 * exceptions to EL2 so a guest cannot mask the host's interrupt source.
	 */
	run->avr_hcr_el2 = HCR_VM | HCR_RW | HCR_TWI | HCR_TWE |
	    HCR_API | HCR_APK | HCR_IMO | HCR_FMO | HCR_AMO;
	/*
	 * VI presents the CPU's virtual IRQ input.  The CPU takes the exception
	 * only when PSTATE.I permits it, and masks IRQs as part of exception
	 * entry.  VI stays level-triggered until userland lowers it with a zero
	 * VMM_IOC_INTR.  A future userland GIC will provide INTIDs, priority,
	 * acknowledge, and EOI behavior; none of those are synthesized here.
	 */
	if (READ_ONCE(vcpu->vc_intr) != 0)
		run->avr_hcr_el2 |= HCR_VI;
	if (arm64_has_el2 == 2)
		run->avr_hcr_el2 |= HCR_E2H;
	run->avr_exit = ARM64_VMM_EXIT_NONE;
	run->avr_cntvoff_el2 = 0;
}

static void
arm64_vmm_save_run(struct vcpu *vcpu)
{
	struct arm64_vmm_run *run = (struct arm64_vmm_run *)vcpu->vc_control_va;
	struct vcpu_reg_state *vrs = &vcpu->vc_regs;

	memcpy(vrs->vrs_gprs, run->avr_gprs, sizeof(vrs->vrs_gprs));
	vrs->vrs_sp = run->avr_sp;
	vrs->vrs_sp_el0 = run->avr_sp_el0;
	vrs->vrs_pc = run->avr_pc;
	vrs->vrs_pstate = run->avr_pstate;
	vrs->vrs_elr_el1 = run->avr_elr_el1;
	vrs->vrs_spsr_el1 = run->avr_spsr_el1;
	vrs->vrs_esr_el1 = run->avr_esr_el1;
	vrs->vrs_far_el1 = run->avr_far_el1;
	vrs->vrs_sctlr_el1 = run->avr_sctlr_el1;
	vrs->vrs_tcr_el1 = run->avr_tcr_el1;
	vrs->vrs_ttbr0_el1 = run->avr_ttbr0_el1;
	vrs->vrs_ttbr1_el1 = run->avr_ttbr1_el1;
	vrs->vrs_mair_el1 = run->avr_mair_el1;
	vrs->vrs_vbar_el1 = run->avr_vbar_el1;
	vrs->vrs_contextidr_el1 = run->avr_contextidr_el1;
	vrs->vrs_cpacr_el1 = run->avr_cpacr_el1;
	vrs->vrs_tpidr_el0 = run->avr_tpidr_el0;
	vrs->vrs_tpidrro_el0 = run->avr_tpidrro_el0;
	vrs->vrs_tpidr_el1 = run->avr_tpidr_el1;
	memcpy(&vcpu->vc_exit.vrs, vrs, sizeof(*vrs));
	/* ESR/FAR/HPFAR describe the exception that caused this EL2 exit. */
	vcpu->vc_exit.vesr = run->avr_esr_el2;
	vcpu->vc_exit.vfar = run->avr_far_el2;
	vcpu->vc_exit.vhpfar = run->avr_hpfar_el2;
}

int
vm_run(struct vm_run_params *vrp)
{
	struct arm64_vmm_run *run;
	struct vm *vm;
	struct vcpu *vcpu;
	paddr_t ipa, last_ipa = (paddr_t)-1;
	uint64_t ec;
	u_int old, retries = 0;
	int error = 0;

	error = vm_find(vrp->vrp_vm_id, &vm);
	if (error)
		return (error);
	vcpu = vm_find_vcpu(vm, vrp->vrp_vcpu_id);
	if (vcpu == NULL) {
		error = ENOENT;
		goto out;
	}

	rw_enter_write(&vcpu->vc_lock);
	old = atomic_cas_uint(&vcpu->vc_state, VCPU_STATE_STOPPED,
	    VCPU_STATE_RUNNING);
	if (old != VCPU_STATE_STOPPED) {
		error = EBUSY;
		goto out_unlock;
	}

	if (copyin(vrp->vrp_exit, &vcpu->vc_exit,
	    sizeof(vcpu->vc_exit)) != 0) {
		error = EFAULT;
		goto out_stopped;
	}

	run = (struct arm64_vmm_run *)vcpu->vc_control_va;
	WRITE_ONCE(vcpu->vc_curcpu, curcpu());
	/*
	 * With guest stage 1 disabled, PC is itself the IPA.  Install its
	 * initial 16KB stage-2 mapping before attempting the first instruction.
	 */
	if ((vcpu->vc_regs.vrs_sctlr_el1 & SCTLR_M) == 0) {
		error = arm64_vmm_fault_page(vcpu, vcpu->vc_regs.vrs_pc);
		if (error) {
			WRITE_ONCE(vcpu->vc_curcpu, NULL);
			goto out_stopped;
		}
	}
	vrp->vrp_exit_reason = VM_EXIT_NONE;
	for (;;) {
		/* Materialize the saved vCPU state in the EL2 run page. */
		arm64_vmm_load_run(vcpu);
		WRITE_ONCE(vcpu->vc_curcpu, curcpu());
		if (arm64_has_el2 == 2)
			arm64_vmm_enter_vhe(vcpu->vc_control_va);
		else
			arm64_vmm_enter_nvhe(vcpu->vc_control_pa);
		/*
		 * EL2 has executed the requested TLBI even if a pending host IRQ
		 * prevented the guest from retiring an instruction.  Do not repeat
		 * this expensive operation until a stage-2 mapping changes.
		 */
		run->avr_flush_tlb = 0;
		arm64_vmm_save_run(vcpu);
		/*
		 * The EL2 vector has restored the host context without acknowledging
		 * the interrupt.  Its normal host handler runs before execution gets
		 * back here.  Returning VM_EXIT_NONE on every physical IRQ/FIQ gives
		 * the scheduler and pending signals an unconditional opportunity to
		 * run; userland simply retries VMM_IOC_RUN.
		 */
		if (run->avr_exit == ARM64_VMM_EXIT_IRQ ||
		    run->avr_exit == ARM64_VMM_EXIT_FIQ)
			break;
		/* SError is host-fatal until recovery semantics are designed. */
		if (run->avr_exit == ARM64_VMM_EXIT_SERROR)
			panic("%s: SError while running vcpu", __func__);
		if (run->avr_exit != ARM64_VMM_EXIT_SYNC)
			panic("%s: unknown EL2 exit %llu", __func__,
			    run->avr_exit);
		ec = ESR_ELx_EXCEPTION(run->avr_esr_el2);

		if (ec == EXCP_INSN_ABORT_L || ec == EXCP_DATA_ABORT_L) {
			/* HPFAR supplies IPA[47:12], while FAR supplies its offset. */
			ipa = ((run->avr_hpfar_el2 & HPFAR_FIPA_MASK) << 8) |
			    (run->avr_far_el2 & PAGE_MASK);
			/* Bound a broken mapping/fault loop before returning to userland. */
			if (ipa == last_ipa) {
				if (++retries > 4) {
					vrp->vrp_exit_reason = VM_EXIT_EXCEPTION;
					break;
				}
			} else {
				last_ipa = ipa;
				retries = 0;
			}
			error = arm64_vmm_fault_page(vcpu, ipa);
			if (error == 0)
				continue;
			vrp->vrp_exit_reason = VM_EXIT_EXCEPTION;
			error = 0;
			break;
		}
		if (ec == EXCP_HVC64)
			vrp->vrp_exit_reason = VM_EXIT_HVC;
		else if (ec == EXCP_WFX)
			vrp->vrp_exit_reason = VM_EXIT_WFX;
		else
			vrp->vrp_exit_reason = VM_EXIT_EXCEPTION;
		break;
	}
	WRITE_ONCE(vcpu->vc_curcpu, NULL);

	/* Tell userland whether a raised virtual IRQ can be taken on re-entry. */
	vrp->vrp_irqready = (vcpu->vc_regs.vrs_pstate & PSR_I) == 0;
	if (copyout(&vcpu->vc_exit, vrp->vrp_exit,
	    sizeof(vcpu->vc_exit)) != 0)
		error = EFAULT;
out_stopped:
	vcpu->vc_state = VCPU_STATE_STOPPED;
out_unlock:
	rw_exit_write(&vcpu->vc_lock);
out:
	refcnt_rele_wake(&vm->vm_refcnt);
	return (error);
}
