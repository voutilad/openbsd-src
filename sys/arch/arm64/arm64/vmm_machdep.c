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
#include <machine/fpu.h>
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
#define VMPIDR_RES1		(1UL << 31)
#define VMM_S2_PAGE_SIZE	(4 * PAGE_SIZE)
#define VMM_S2_PAGE_MASK	(VMM_S2_PAGE_SIZE - 1)
#define VMM_S2_FAULT_SIZE	(256 * 1024)
#define VMM_ASYNC_RETRIES	8

/* Atomic software form of the one interrupt selected by userland. */
#define VMM_INTR_VALID		(1UL << 63)
#define VMM_INTR_INTID_MASK	0xffffUL
#define VMM_INTR_PRIORITY_SHIFT	16
#define VMM_INTR_PRIORITY_MASK	(0xffUL << VMM_INTR_PRIORITY_SHIFT)

/* CNTHCTL_EL2.EL1PCTEN moves from bit 0 to bit 10 when E2H is set. */
#define CNTHCTL_EL1PCTEN_VHE	(1 << 10)

static struct vcpu *arm64_vmm_resident[MAXCPUS];

int	arm64_vmm_enter_nvhe(paddr_t);
int	arm64_vmm_enter_vhe(vaddr_t, int, int, int);

static int	arm64_vmm_fault_page(struct vcpu *, paddr_t);
static int	arm64_vmm_alloc_memory(struct vm *);
static vaddr_t	arm64_vmm_translate_gpa(struct vm *, paddr_t);
static int	arm64_vmm_intr_pending(struct vm_intr_params *);
static int	arm64_vmm_irqcfg(struct vm_irqcfg_params *);
static int	arm64_vmm_defer_host_timer(struct arm64_vmm_run *);
static void	arm64_vmm_load_lr(uint64_t *, uint64_t);
static void	arm64_vmm_load_intr(struct vcpu *);
static void	arm64_vmm_load_run(struct vcpu *, int);
static void	arm64_vmm_save_run(struct vcpu *);

CTASSERT(sizeof(struct arm64_vmm_run) <= PAGE_SIZE);

/*
 * Mask the VHE host's hyp-virtual timer and remember its absolute deadline.
 *
 * This is deliberately done in C at the beginning of a recovery retry,
 * while ordinary host interrupt dispatch is still installed.  A nested EL2
 * context switch makes even arm64_vmm_load_run() and fpu_kernel_enter() long
 * enough for the next randomized host tick to become pending.  Masking the
 * source later in the assembly entry path cannot withdraw a level PPI which
 * the outer GIC has already latched.
 *
 * The assembly entry gives the masked timer a temporary relative deadline
 * immediately before ERET.  Its exit path restores the saved CVAL and CTL,
 * so any host clock work which became due is delivered normally afterward.
 */
static int
arm64_vmm_defer_host_timer(struct arm64_vmm_run *run)
{
	uint64_t ctl, cval;

	__asm volatile("mrs %x0, s3_4_c14_c3_1" : "=r" (ctl));
	if ((ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK)) != CNTV_CTL_ENABLE)
		return (0);

	__asm volatile("msr s3_4_c14_c3_1, %x0; isb" ::
	    "r" (ctl | CNTV_CTL_IMASK) : "memory");
	__asm volatile("mrs %x0, s3_4_c14_c3_2" : "=r" (cval));
	run->avr_host_cnthv_ctl_el2 = ctl;
	run->avr_host_cnthv_cval_el2 = cval;
	return (1);
}

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
	case VMM_IOC_IRQCFG:
		return (arm64_vmm_irqcfg((struct vm_irqcfg_params *)data));
	default:
		return (ENOTTY);
	}
}

/*
 * Configure the GIC properties of an interrupt whose signal originates in
 * vmm(4), rather than in a userland device model.  The architectural virtual
 * timer is the first such source.  Its timer registers remain hardware state;
 * this ioctl supplies only the Distributor/Redistributor policy which vmd
 * learned from guest MMIO.
 */
static int
arm64_vmm_irqcfg(struct vm_irqcfg_params *viq)
{
	struct vm *vm;
	struct vcpu *vcpu;
	int error, ret = 0;

	error = vm_find(viq->viq_vm_id, &vm);
	if (error != 0)
		return (error);
	vcpu = vm_find_vcpu(vm, viq->viq_vcpu_id);
	if (vcpu == NULL) {
		ret = ENOENT;
		goto out;
	}
	if (viq->viq_intr != VMM_ARM64_TIMER_INTID ||
	    (viq->viq_flags & ~VMM_IRQCFG_ENABLED) != 0) {
		ret = EINVAL;
		goto out;
	}

	if ((viq->viq_flags & VMM_IRQCFG_ENABLED) != 0)
		WRITE_ONCE(vcpu->vc_timer_intr, VMM_INTR_VALID |
		    ((uint64_t)viq->viq_priority << VMM_INTR_PRIORITY_SHIFT) |
		    viq->viq_intr);
	else
		WRITE_ONCE(vcpu->vc_timer_intr, 0);
out:
	refcnt_rele_wake(&vm->vm_refcnt);
	return (ret);
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
	if (vip->vip_intr > VMM_INTR_MAX ||
	    vip->vip_level > VMM_INTR_LEVEL_HIGH) {
		ret = EINVAL;
		goto out;
	}

	/*
	 * Userland still owns the distributor and redistributor.  This ioctl
	 * communicates only their currently selected level interrupt to the
	 * hardware virtual CPU interface.  Packing identity, priority, and level
	 * into one naturally aligned word gives vm_run() a coherent snapshot while
	 * this ioctl interrupts a running vCPU without taking vc_lock.
	 *
	 * A low level is represented by zero.  A high level carries the real INTID,
	 * so guest IAR and EOIR accesses can execute in hardware rather than being
	 * decoded by either vmm(4) or vmd(8).
	 */
	if (vip->vip_level == VMM_INTR_LEVEL_HIGH)
		WRITE_ONCE(vcpu->vc_intr, VMM_INTR_VALID |
		    ((uint64_t)vip->vip_priority << VMM_INTR_PRIORITY_SHIFT) |
		    vip->vip_intr);
	else
		WRITE_ONCE(vcpu->vc_intr, 0);
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
	if (com == VMM_IOC_INTR || com == VMM_IOC_IRQCFG)
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
	vcpu->vc_lastcpu = NULL;
	vcpu->vc_el12_dirty = 1;
	vcpu->vc_exit_pending = 0;
	vcpu->vc_entry_recovery = 0;
	rw_init(&vcpu->vc_lock, "vcpu");
	vcpu->vc_regs.vrs_pstate = PSR_F | PSR_I | PSR_A | PSR_D |
	    PSR_M_EL1h;
	vcpu->vc_regs.vrs_sctlr_el1 = SCTLR_RES1;
	run = (struct arm64_vmm_run *)vcpu->vc_control_va;
	/* The guest establishes PMR, BPR, and Group-1 enable through ICC registers. */
	run->avr_ich_vmcr_el2 = 0;
	run->avr_cntv_ctl_el0 = 0;
	run->avr_cntv_cval_el0 = 0;
	run->avr_cntvct_el0 = 0;
	vcpu->vc_timer_ctl = 0;
	vcpu->vc_timer_deadline = 0;
	WRITE_ONCE(vcpu->vc_timer_intr, 0);
	/* A recycled VMID must not inherit translations from an older VM. */
	run->avr_flush_tlb = 1;
	return (0);
}

void
vcpu_deinit(struct vcpu *vcpu)
{
	int i;

	for (i = 0; i < MAXCPUS; i++) {
		if (READ_ONCE(arm64_vmm_resident[i]) == vcpu)
			WRITE_ONCE(arm64_vmm_resident[i], NULL);
	}
	if (vcpu->vc_control_va != 0) {
		km_free((void *)vcpu->vc_control_va, PAGE_SIZE, &kv_page,
		    &kp_zero);
		vcpu->vc_control_va = 0;
	}
}

int
vcpu_reset_regs(struct vcpu *vcpu, struct vcpu_reg_state *vrs)
{
	struct arm64_vmm_run *run;

	if ((vrs->vrs_pc & (INSN_SIZE - 1)) != 0)
		return (EINVAL);
	if ((vrs->vrs_pstate & PSR_M_MASK) != PSR_M_EL1h)
		return (EINVAL);

	memcpy(&vcpu->vc_regs, vrs, sizeof(vcpu->vc_regs));
	vcpu->vc_lastcpu = NULL;
	vcpu->vc_el12_dirty = 1;
	vcpu->vc_exit_pending = 0;
	vcpu->vc_entry_recovery = 0;
	if (vcpu->vc_regs.vrs_sctlr_el1 == 0)
		vcpu->vc_regs.vrs_sctlr_el1 = SCTLR_RES1;
	/* RESETCPU starts a fresh architectural context, including FP/AdvSIMD. */
	run = (struct arm64_vmm_run *)vcpu->vc_control_va;
	memset(run->avr_fp, 0, sizeof(run->avr_fp));
	run->avr_fpcr = 0;
	run->avr_fpsr = 0;
	memset(run->avr_pauth, 0, sizeof(run->avr_pauth));
	run->avr_ich_vmcr_el2 = 0;
	run->avr_ich_lr0_el2 = 0;
	run->avr_ich_lr1_el2 = 0;
	run->avr_cntv_ctl_el0 = 0;
	run->avr_cntv_cval_el0 = 0;
	run->avr_cntvct_el0 = 0;
	WRITE_ONCE(vcpu->vc_intr, 0);
	WRITE_ONCE(vcpu->vc_timer_intr, 0);
	vcpu->vc_timer_ctl = 0;
	vcpu->vc_timer_deadline = 0;
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
	else {
		if (memcmp(&vcpu->vc_regs.vrs_elr_el1,
		    &vrwp->vrwp_regs.vrs_elr_el1,
		    offsetof(struct vcpu_reg_state, vrs_tpidr_el0) -
		    offsetof(struct vcpu_reg_state, vrs_elr_el1)) != 0)
			vcpu->vc_el12_dirty = 1;
		memcpy(&vcpu->vc_regs, &vrwp->vrwp_regs,
		    sizeof(vcpu->vc_regs));
	}
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
	struct vm_mem_range *vmr = NULL;
	paddr_t hpa, ipa, ipa_base, ipa_end, mapped, pa;
	vaddr_t hva;
	int error, i;

	ipa = trunc_page(gpa);

	if (pmap_extract(vcpu->vc_parent->vm_pmap, ipa, &mapped))
		return (0);

	/*
	 * Locate the RAM slot containing the fault before calculating the
	 * read-ahead window.  Clipping the window to that slot prevents a fault
	 * near its edge from wiring an adjacent MMIO hole or a different object.
	 */
	for (i = 0; i < vcpu->vc_parent->vm_nmemranges; i++) {
		vmr = &vcpu->vc_parent->vm_memranges[i];
		if (ipa >= vmr->vmr_gpa &&
		    ipa < vmr->vmr_gpa + vmr->vmr_size)
			break;
	}
	if (i == vcpu->vc_parent->vm_nmemranges ||
	    vmr->vmr_type == VM_MEM_MMIO)
		return (EFAULT);

	/*
	 * The backing allocator supplies physically contiguous, aligned 16KB
	 * groups.  Map up to 256KB of those groups for each stage-2 fault.  Early
	 * arm64 bootstrap touches page-table metadata sequentially; installing
	 * only the faulting group would otherwise require one nested exception
	 * and one VM-wide stage-2 TLBI every 16KB.  The larger bounded window
	 * amortizes those exits without wiring untouched RAM for the whole VM.
	 */
	ipa_base = ipa & ~(VMM_S2_FAULT_SIZE - 1);
	if (ipa_base < vmr->vmr_gpa)
		ipa_base = vmr->vmr_gpa;
	ipa_end = ipa_base + VMM_S2_FAULT_SIZE;
	if (ipa_end > vmr->vmr_gpa + vmr->vmr_size)
		ipa_end = vmr->vmr_gpa + vmr->vmr_size;
	KASSERT((ipa_base & VMM_S2_PAGE_MASK) == 0);
	KASSERT((ipa_end & VMM_S2_PAGE_MASK) == 0);

	hva = arm64_vmm_translate_gpa(vcpu->vc_parent, ipa_base);
	if (hva == 0)
		return (EFAULT);

	error = uvm_fault_wire(&p->p_vmspace->vm_map, hva,
	    hva + (ipa_end - ipa_base), PROT_READ | PROT_WRITE);
	if (error)
		return (error);
	for (pa = ipa_base; pa < ipa_end; pa += PAGE_SIZE, hva += PAGE_SIZE) {
		if (!pmap_extract(p->p_vmspace->vm_map.pmap,
		    hva, &hpa))
			return (EFAULT);
		/* pmap_enter() cannot wait while holding the pmap lock. */
		pmap_populate(vcpu->vc_parent->vm_pmap, pa);
		error = pmap_enter(vcpu->vc_parent->vm_pmap,
		    pa, hpa,
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
arm64_vmm_load_lr(uint64_t *lrp, uint64_t signal)
{
	uint64_t intid, lr, state;

	/*
	 * An LR is architectural state, not a level latch which can be overwritten
	 * on every entry.  An active LR must retain its INTID until the guest
	 * executes EOIR.  Its source may lower in the meantime; a different signal
	 * can only occupy this slot after hardware reports the LR invalid.
	 *
	 * A merely pending LR has not been acknowledged and is safe to withdraw or
	 * replace.  This is what lets a device deassert before IAR without leaving
	 * a phantom interrupt in the virtual CPU interface.
	 */
	lr = *lrp;
	state = lr & ICH_LR_STATE_MASK;
	if (state == ICH_LR_ACTIVE ||
	    state == (ICH_LR_ACTIVE | ICH_LR_PENDING)) {
		/* Never retain a speculative re-pend after the selected line moved. */
		*lrp = lr & ~ICH_LR_PENDING;
		return;
	}
	if ((signal & VMM_INTR_VALID) == 0) {
		*lrp = 0;
		return;
	}

	intid = signal & VMM_INTR_INTID_MASK;
	*lrp = ICH_LR_PENDING | ICH_LR_GROUP1 | intid |
	    (((signal & VMM_INTR_PRIORITY_MASK) >> VMM_INTR_PRIORITY_SHIFT) <<
	    ICH_LR_PRIORITY_SHIFT);
}

static void
arm64_vmm_load_intr(struct vcpu *vcpu)
{
	struct arm64_vmm_run *run = (struct arm64_vmm_run *)vcpu->vc_control_va;
	uint64_t now, signal, timer;

	/* LR0 is the interrupt selected by vmd's userland device model. */
	signal = READ_ONCE(vcpu->vc_intr);
	arm64_vmm_load_lr(&run->avr_ich_lr0_el2, signal);

	/*
	 * A VHE guest programs CNTV_* directly.  Existing host clock exits sample
	 * that bank often enough for a running vCPU; a trapped WFI is woken at the
	 * exact deadline by vmd.  Compare in the host counter domain established
	 * when the timer state was saved.  LR1 remains independent of LR0 so the
	 * hardware CPU interface can apply priority and preemption while either
	 * interrupt is active.
	 */
	timer = READ_ONCE(vcpu->vc_timer_intr);
	if ((timer & VMM_INTR_VALID) != 0 &&
	    (vcpu->vc_timer_ctl &
	    (CNTV_CTL_ENABLE | CNTV_CTL_IMASK)) == CNTV_CTL_ENABLE) {
		now = READ_SPECIALREG(cntvct_el0);
		if ((int64_t)(now - vcpu->vc_timer_deadline) < 0)
			timer = 0;
	} else
		timer = 0;
	arm64_vmm_load_lr(&run->avr_ich_lr1_el2, timer);
}

static void
arm64_vmm_load_run(struct vcpu *vcpu, int load_intr)
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
	run->avr_cntkctl_el1 = vrs->vrs_cntkctl_el1;
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
	/*
	 * A uniprocessor guest still migrates between physical host CPUs.  Force
	 * its local cache/TLB maintenance to the inner-shareable domain and
	 * upgrade its barriers accordingly; otherwise a translation-fault entry
	 * left on one host CPU can reappear after the vCPU migrates back to it.
	 * Leave guest stage-1 TLB maintenance untrapped.  It executes directly on
	 * bare metal, while an outer hypervisor providing virtual EL2 associates
	 * the architectural operation with this HCR/VTTBR context.  vmm therefore
	 * neither decodes nor advances a guest TLBI instruction.
	 */
	run->avr_hcr_el2 = HCR_VM | HCR_RW | HCR_TWI | HCR_TWE |
	    HCR_API | HCR_APK | HCR_IMO | HCR_FMO | HCR_AMO |
	    HCR_FB | HCR_BSU_IS;
	/*
	 * HCR_EL2.IMO makes Non-secure EL1 Group 1 ICC accesses select the
	 * virtual CPU interface.  Leave TALL1 and TC clear: the guest's IAR, EOIR,
	 * PMR, BPR, and Group-1 enable instructions then operate on ICH state in
	 * hardware.  vmm(4) only context-switches that state and never decodes an
	 * ICC instruction.
	 */
	run->avr_ich_hcr_el2 = ICH_HCR_EN;
	/*
	 * Reconcile the last hardware LR state with userland's selected level on
	 * every entry.  This is required even when the selected signal did not
	 * change: after an EOIR or WFI exit an invalid LR must be refilled for a
	 * still-asserted level interrupt.  load_intr is retained in the entry ABI
	 * for the surrounding nested-retry policy but no longer gates LR refill.
	 */
	(void)load_intr;
	arm64_vmm_load_intr(vcpu);
	if (arm64_has_el2 == 2)
		run->avr_hcr_el2 |= HCR_E2H;
	run->avr_exit = ARM64_VMM_EXIT_NONE;
	run->avr_cntvoff_el2 = 0;
	/*
	 * MPIDR_EL1 must agree with the affinity encoded in the guest's CPU
	 * description.  The initial uniprocessor ABI assigns Aff0 == 0; retaining
	 * the RES1 bit gives the guest the architecturally valid MPIDR value
	 * 0x80000000.  Using vc_id for Aff0 also makes this state ready for the
	 * first small-SMP extension without exposing whichever physical host CPU
	 * happened to execute this vCPU.
	 */
	run->avr_vmpidr_el2 = VMPIDR_RES1 | vcpu->vc_id;
	/*
	 * A VHE host has a separate EL02 virtual-timer bank for its EL1 guest, so
	 * leave EL1TVT clear and let CNTV_* execute directly.  An nVHE host shares
	 * its virtual-timer bank with the guest; retain a trapped userland fallback
	 * there until that host bank is safely context-switched.  Both modes leave
	 * the physical timer inaccessible and keep the counter readable.
	 */
	if (arm64_has_el2 == 2)
		run->avr_cnthctl_el2 = CNTHCTL_EL1PCTEN_VHE;
	else
		run->avr_cnthctl_el2 = CNTHCTL_EL1PCTEN | CNTHCTL_EL1TVT;
}

static void
arm64_vmm_save_run(struct vcpu *vcpu)
{
	struct arm64_vmm_run *run = (struct arm64_vmm_run *)vcpu->vc_control_va;
	struct vcpu_reg_state *vrs = &vcpu->vc_regs;
	int64_t delta;
	uint64_t now;

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
	vrs->vrs_cntkctl_el1 = run->avr_cntkctl_el1;
	vrs->vrs_tpidr_el0 = run->avr_tpidr_el0;
	vrs->vrs_tpidrro_el0 = run->avr_tpidrro_el0;
	vrs->vrs_tpidr_el1 = run->avr_tpidr_el1;
	memcpy(&vcpu->vc_exit.vrs, vrs, sizeof(*vrs));
	/* ESR/FAR/HPFAR describe the exception that caused this EL2 exit. */
	vcpu->vc_exit.vesr = run->avr_esr_el2;
	vcpu->vc_exit.vfar = run->avr_far_el2;
	vcpu->vc_exit.vhpfar = run->avr_hpfar_el2;
	vcpu->vc_exit.vet_cntv_ctl = run->avr_cntv_ctl_el0;
	vcpu->vc_exit.vet_cntv_cval = run->avr_cntv_cval_el0;
	vcpu->vc_exit.vet_cntvct = run->avr_cntvct_el0;
	vcpu->vc_exit.vet_flags = 0;
	if (arm64_has_el2 == 2) {
		/*
		 * Convert CVAL into the host counter domain using the relative
		 * distance sampled by assembly at this exit.  An outer hypervisor's
		 * counter offset therefore cancels, and the result survives migration
		 * of the vCPU to another physical CPU.
		 */
		delta = (int64_t)(run->avr_cntv_cval_el0 -
		    run->avr_cntvct_el0);
		now = READ_SPECIALREG(cntvct_el0);
		vcpu->vc_timer_deadline = delta > 0 ? now + delta : now;
		vcpu->vc_timer_ctl = run->avr_cntv_ctl_el0;
		vcpu->vc_exit.vet_flags = VMM_TIMER_F_HARDWARE;
	}
}

int
vm_run(struct vm_run_params *vrp)
{
	struct arm64_vmm_run *run;
	struct vm *vm;
	struct vcpu *vcpu;
	struct cpu_info *entry_ci;
	paddr_t ipa, last_ipa = (paddr_t)-1;
	uint64_t ec;
	u_int irq_retries = 0, old, retries = 0;
	int defer_timer, entry_recovery, error = 0, fast_entry;
	int ipi_disabled, recovery;
	int recovery_s;
	int save_async;

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
	/*
	 * A VMM_IOC_RUN which exhausted its bounded asynchronous retries has
	 * returned through the normal syscall and scheduler path.  The outer host
	 * has therefore had an unconditional opportunity to acknowledge and EOI
	 * the interrupt which prevented guest entry.  Consume its recovery token
	 * by protecting the first entry of this new call before any nested EL2
	 * state preparation can race the next clock deadline.
	 */
	entry_recovery = vcpu->vc_entry_recovery;
	vcpu->vc_entry_recovery = 0;
	/*
	 * Like amd64, VMM_IOC_RUN is both the exit-report and exit-completion
	 * interface.  If the previous run asked userland for help, consume the
	 * register snapshot that userland has just copied back.  MMIO and trapped
	 * system-register emulation can therefore advance PC and supply a result
	 * without a separate VMM_IOC_WRITEREGS round trip.
	 *
	 * Do this only after an assisted exit.  The exit buffer is zeroed before
	 * the first RUN and is also copied out for ordinary host-interrupt yields;
	 * neither case represents guest state supplied by userland.
	 */
	if (vcpu->vc_exit_pending) {
		if (memcmp(&vcpu->vc_regs.vrs_elr_el1,
		    &vcpu->vc_exit.vrs.vrs_elr_el1,
		    offsetof(struct vcpu_reg_state, vrs_tpidr_el0) -
		    offsetof(struct vcpu_reg_state, vrs_elr_el1)) != 0)
			vcpu->vc_el12_dirty = 1;
		memcpy(&vcpu->vc_regs, &vcpu->vc_exit.vrs,
		    sizeof(vcpu->vc_regs));
		vcpu->vc_exit_pending = 0;
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
		/*
		 * Protect all of a recovery retry, not only the final assembly
		 * transition.  Under nesting, the C-side state preparation also traps
		 * enough EL2 state through the outer hypervisor to lose a short clock
		 * deadline.  splraise() pins this thread while ordinary devices are
		 * masked, and holding the IPI source keeps a reschedule request pending
		 * until the bounded retry has returned.
		 */
		recovery = entry_recovery || irq_retries >= 1;
		entry_ci = curcpu();
		recovery_s = recovery ? splraise(IPL_CLOCK - 1) : IPL_NONE;
		ipi_disabled = recovery && arm_intr_disable_ipi();
		defer_timer = 0;
		if (recovery && arm64_has_el2 == 2)
			defer_timer = arm64_vmm_defer_host_timer(run);

		/*
		 * Materialize the saved vCPU state in the EL2 run page.  A same-call
		 * retry retains the interrupt signal used by the failed entry.  A new
		 * VMM_IOC_INTR may arrive while a nested hypervisor is still processing
		 * that entry; loading it here would let a short-period guest timer win
		 * every retry before the interrupted instruction can retire.  The
		 * bounded retry delays such a signal only until this ioctl returns, when
		 * the next VMM_IOC_RUN takes a fresh snapshot.
		 */
		arm64_vmm_load_run(vcpu, irq_retries == 0);
		/*
		 * A combined stage-1/stage-2 translation can remain in a physical
		 * CPU's TLB after this vCPU last ran there.  Flush when the vCPU
		 * migrates before reusing that VMID on its new CPU.
		 */
		if (vcpu->vc_lastcpu != NULL && vcpu->vc_lastcpu != curcpu())
			run->avr_flush_tlb = 1;
		WRITE_ONCE(vcpu->vc_curcpu, curcpu());
		/*
		 * A guest may use FP/AdvSIMD whenever its CPACR_EL1 permits it.
		 * Save any live user state through the normal lazy-FPU machinery and
		 * make the vector unit available to the EL2 context switch.  On return,
		 * fpu_kernel_exit() leaves user access trapping so the process reloads
		 * its saved state instead of observing the guest's vector registers.
		 */
		fpu_kernel_enter();
		if (irq_retries != 0)
			fast_entry = 1;
		else if (arm64_has_el2 == 2 && !vcpu->vc_el12_dirty &&
		    vcpu->vc_lastcpu == curcpu() &&
		    READ_ONCE(arm64_vmm_resident[cpu_number()]) == vcpu)
			fast_entry = 2;
		else
			fast_entry = 0;
		save_async = (irq_retries == VMM_ASYNC_RETRIES);
		if (arm64_has_el2 == 2)
			arm64_vmm_enter_vhe(vcpu->vc_control_va,
			    fast_entry, save_async, defer_timer);
		else
			arm64_vmm_enter_nvhe(vcpu->vc_control_pa);
		if (arm64_has_el2 == 2) {
			WRITE_ONCE(arm64_vmm_resident[cpu_number()], vcpu);
			vcpu->vc_lastcpu = curcpu();
			vcpu->vc_el12_dirty = 0;
		}
		if (ipi_disabled) {
			KASSERT(entry_ci == curcpu());
			arm_intr_enable_ipi();
		}
		if (recovery)
			splx(recovery_s);
		fpu_kernel_exit();
		/*
		 * EL2 has executed any requested combined invalidation even if a
		 * pending host IRQ prevented the guest from retiring an instruction.
		 * Do not repeat it until another stage-2 mapping change requires one.
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
		    run->avr_exit == ARM64_VMM_EXIT_FIQ) {
			/*
			 * Retry a short, bounded sequence before returning to userland.
			 * A physical timer can arrive while a nested hypervisor is still
			 * restoring the guest context; if every such exit crosses the
			 * ioctl boundary, the next tick can arrive before the guest
			 * retires even one instruction.  Deferring the host virtual timer
			 * after repeated asynchronous exits prevents that timer from starving
			 * a nested guest during its comparatively expensive entry sequence.
			 * The small bound still covers unrelated asynchronous work without
			 * keeping the calling process in the kernel for an unbounded interval.
			 *
			 * arm64 defers a reschedule requested by an interrupt taken from
			 * system mode until this ioctl returns.  The retry therefore stays
			 * on entry_ci and may reuse that CPU's resident EL12 bank.  The
			 * final asynchronous exit takes the full save path and supplies
			 * the regular userland scheduling point.
			 */
			KASSERT(entry_ci == curcpu());
			if (irq_retries++ < VMM_ASYNC_RETRIES)
				continue;
			/* Protect the first entry after this syscall-level yield. */
			vcpu->vc_entry_recovery = 1;
			break;
		}
		/*
		 * A synchronous exit snapshots EL12 before C handles it.  Handling a
		 * stage-2 fault may sleep, so the following entry must be a full one
		 * even if an asynchronous retry preceded this exception.
		 */
		irq_retries = 0;
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
	else
		vcpu->vc_exit_pending =
		    vrp->vrp_exit_reason != VM_EXIT_NONE;
out_stopped:
	vcpu->vc_state = VCPU_STATE_STOPPED;
out_unlock:
	rw_exit_write(&vcpu->vc_lock);
out:
	refcnt_rele_wake(&vm->vm_refcnt);
	return (error);
}
