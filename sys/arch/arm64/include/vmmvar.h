/*	$OpenBSD: vmmvar.h,v 1.4 2026/09/19 17:21:52 dv Exp $	*/
/*
 * Copyright (c) 2014 Mike Larkin <mlarkin@openbsd.org>
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

/*
 * CPU capabilities for VMM operation
 */
#ifndef _MACHINE_VMMVAR_H_
#define _MACHINE_VMMVAR_H_

#define VMM_HV_SIGNATURE 	"OpenBSDVMM58"

/* Exit Reasons */
#define VM_EXIT_HVC				0x0001
#define VM_EXIT_WFX				0x0002
#define VM_EXIT_EXCEPTION			0x0003
#define VM_EXIT_TERMINATED			0xFFFE
#define VM_EXIT_NONE				0xFFFF

struct vmm_softc_md {
	/* Capabilities */
	uint32_t		nr_cpus;	/* [I] */
};

/*
 * struct vcpu_inject_event	: describes an exception or interrupt to inject.
 */
struct vcpu_inject_event {
	uint8_t		vie_vector;	/* Exception or interrupt vector. */
	uint32_t	vie_errorcode;	/* Optional error code. */
	uint8_t		vie_type;
#define VCPU_INJECT_NONE	0
#define VCPU_INJECT_INTR	1	/* External hardware interrupt. */
#define VCPU_INJECT_EX		2	/* HW or SW Exception */
#define VCPU_INJECT_NMI		3	/* Non-maskable Interrupt */
};

#define VCPU_REGS_NGPRS		31

#define VCPU_REGS_X0		0
#define VCPU_REGS_X1		1
#define VCPU_REGS_X2		2
#define VCPU_REGS_X3		3
#define VCPU_REGS_X4		4
#define VCPU_REGS_X5		5
#define VCPU_REGS_X6		6
#define VCPU_REGS_X7		7
#define VCPU_REGS_X8		8
#define VCPU_REGS_X9		9
#define VCPU_REGS_X10		10
#define VCPU_REGS_X11		11
#define VCPU_REGS_X12		12
#define VCPU_REGS_X13		13
#define VCPU_REGS_X14		14
#define VCPU_REGS_X15		15
#define VCPU_REGS_X16		16
#define VCPU_REGS_X17		17
#define VCPU_REGS_X18		18
#define VCPU_REGS_X19		19
#define VCPU_REGS_X20		20
#define VCPU_REGS_X21		21
#define VCPU_REGS_X22		22
#define VCPU_REGS_X23		23
#define VCPU_REGS_X24		24
#define VCPU_REGS_X25		25
#define VCPU_REGS_X26		26
#define VCPU_REGS_X27		27
#define VCPU_REGS_X28		28
#define VCPU_REGS_X29		29
#define VCPU_REGS_X30		30

struct vcpu_reg_state {
	/* Integer and exception-entry state used to resume the guest. */
	uint64_t			vrs_gprs[VCPU_REGS_NGPRS];
	uint64_t			vrs_sp;		/* SP_EL1 (EL1h) */
	uint64_t			vrs_sp_el0;	/* SP_EL0 */
	uint64_t			vrs_pc;		/* ELR_EL2 */
	uint64_t			vrs_pstate;	/* SPSR_EL2 */

	/* EL1's exception-return and syndrome state. */
	uint64_t			vrs_elr_el1;
	uint64_t			vrs_spsr_el1;
	uint64_t			vrs_esr_el1;
	uint64_t			vrs_far_el1;

	/* EL1 stage-1 translation and execution controls. */
	uint64_t			vrs_sctlr_el1;
	uint64_t			vrs_tcr_el1;
	uint64_t			vrs_ttbr0_el1;
	uint64_t			vrs_ttbr1_el1;
	uint64_t			vrs_mair_el1;
	uint64_t			vrs_vbar_el1;
	uint64_t			vrs_contextidr_el1;
	uint64_t			vrs_cpacr_el1;
	uint64_t			vrs_cntkctl_el1;

	/* Thread identifiers visible at EL0 and EL1. */
	uint64_t			vrs_tpidr_el0;
	uint64_t			vrs_tpidrro_el0;
	uint64_t			vrs_tpidr_el1;
};

/*
 * struct vm_exit
 *
 * Contains VM exit information communicated to vmd(8). This information is
 * gathered by vmm(4) from the CPU on each exit that requires help from vmd.
 */
struct vm_exit {
	struct vcpu_reg_state		vrs;
	uint64_t			vesr;
	uint64_t			vfar;
	uint64_t			vhpfar;
	/* Architectural virtual-timer state sampled at this exit. */
	uint64_t			vet_cntv_ctl;
	uint64_t			vet_cntv_cval;
	uint64_t			vet_cntvct;
	uint32_t			vet_flags;
};

#define VMM_TIMER_F_HARDWARE	(1U << 0)

struct vm_intr_params {
	/* Input parameters to VMM_IOC_INTR */
	uint32_t		vip_vcpu_id;
	uint16_t		vip_intr;	/* GIC virtual INTID */
	uint8_t			vip_priority;	/* GIC priority byte */
	uint8_t			vip_level;	/* VMM_INTR_LEVEL_* */
};

/* INTIDs 1020 through 1023 are reserved for special GIC responses. */
#define VMM_INTR_MAX		1019
#define VMM_INTR_LEVEL_LOW	0
#define VMM_INTR_LEVEL_HIGH	1

struct vm_irqcfg_params {
	/* Input parameters to VMM_IOC_IRQCFG. */
	uint32_t		viq_vm_id;
	uint32_t		viq_vcpu_id;
	uint16_t		viq_intr;
	uint8_t			viq_priority;
	uint8_t			viq_flags;
};

#define VMM_IRQCFG_ENABLED	(1U << 0)
#define VMM_ARM64_TIMER_INTID	27

#define VM_RWREGS_GPRS	0x1	/* read/write GPRs */
#define VM_RWREGS_ALL	(VM_RWREGS_GPRS)

struct vm_rwregs_params {
	/*
	 * Input/output parameters to VMM_IOC_READREGS /
	 * VMM_IOC_WRITEREGS
	 */
	uint32_t		vrwp_vcpu_id;
	uint64_t		vrwp_mask;
	struct vcpu_reg_state	vrwp_regs;
};

enum {
	VEI_DIR_OUT,
	VEI_DIR_IN
};

/* IOCTL definitions */
#define VMM_IOC_INTR _IOW('V', 6, struct vm_intr_params) /* Intr pending */
#define VMM_IOC_IRQCFG _IOW('V', 12, struct vm_irqcfg_params)

#ifdef _KERNEL

#include <sys/queue.h>
#include <sys/rwlock.h>

#define ARM64_VMM_MODE_NVHE	1
#define ARM64_VMM_MODE_VHE	2

/* Private EL2-to-C exit reasons, distinct from the public VM_EXIT_* ABI. */
#define ARM64_VMM_EXIT_NONE	0
#define ARM64_VMM_EXIT_SYNC	1	/* guest synchronous exception */
#define ARM64_VMM_EXIT_IRQ	2	/* physical host IRQ */
#define ARM64_VMM_EXIT_FIQ	3	/* physical host FIQ */
#define ARM64_VMM_EXIT_SERROR	4	/* physical SError */

enum {
	VMM_MODE_UNKNOWN,
	VMM_MODE_STAGE2
};

enum {
	VMM_MEM_TYPE_REGULAR,
	VMM_MEM_TYPE_MMIO,
	VMM_MEM_TYPE_UNKNOWN
};

struct vm;
struct vm_create_params;
struct cpu_info;
struct device;
struct proc;

/*
 * State shared with the EL2 exception vectors.  This structure is allocated
 * from a physically contiguous page because EL2 runs with its stage-1 MMU
 * disabled.  Keep the assembly offsets in genassym.cf in sync.
 */
struct arm64_vmm_run {
	/* Guest state copied from struct vcpu_reg_state before entry. */
	uint64_t	avr_gprs[VCPU_REGS_NGPRS];
	uint64_t	avr_sp;
	uint64_t	avr_sp_el0;
	uint64_t	avr_pc;
	uint64_t	avr_pstate;
	uint64_t	avr_elr_el1;
	uint64_t	avr_spsr_el1;
	uint64_t	avr_esr_el1;
	uint64_t	avr_far_el1;
	uint64_t	avr_sctlr_el1;
	uint64_t	avr_tcr_el1;
	uint64_t	avr_ttbr0_el1;
	uint64_t	avr_ttbr1_el1;
	uint64_t	avr_mair_el1;
	uint64_t	avr_vbar_el1;
	uint64_t	avr_contextidr_el1;
	uint64_t	avr_cpacr_el1;
	uint64_t	avr_cntkctl_el1;
	uint64_t	avr_tpidr_el0;
	uint64_t	avr_tpidrro_el0;
	uint64_t	avr_tpidr_el1;

	/*
	 * FP/AdvSIMD is private kernel state for now: the initial vmm(4) ABI
	 * exposes only integer registers, but a guest must retain all 32 128-bit
	 * vector registers across exits.  Keep the first register 16-byte aligned
	 * so the EL2 pair loads and stores are naturally aligned.
	 */
	uint64_t	avr_fp[64] __aligned(16);
	uint64_t	avr_fpcr;
	uint64_t	avr_fpsr;

	/* APIA, APIB, APDA, APDB, and APGA, each stored low word first. */
	uint64_t	avr_pauth[10] __aligned(16);

	/* Syndrome state captured by the EL2 vectors on a guest exit. */
	uint64_t	avr_esr_el2;
	uint64_t	avr_far_el2;
	uint64_t	avr_hpfar_el2;

	/* EL2 controls constructed by arm64_vmm_load_run(). */
	uint64_t	avr_vttbr_el2;
	uint64_t	avr_vtcr_el2;
	uint64_t	avr_hcr_el2;
	uint64_t	avr_ich_hcr_el2;
	/* Guest virtual CPU-interface state retained across exits. */
	uint64_t	avr_ich_vmcr_el2;
	/* LR0 carries userland devices; LR1 is reserved for the virtual timer. */
	uint64_t	avr_ich_lr0_el2;
	uint64_t	avr_ich_lr1_el2;
	uint64_t	avr_mode;
	uint64_t	avr_exit;
	/* Pending vmm-owned combined stage-1/stage-2 invalidation. */
	uint64_t	avr_flush_tlb;
	/* Guest virtual timer bank used directly by a VHE EL1 guest. */
	uint64_t	avr_cntv_ctl_el0;
	uint64_t	avr_cntv_cval_el0;
	uint64_t	avr_cntvct_el0;
	uint64_t	avr_cntvoff_el2;
	/* Select direct virtual-timer access or the nVHE software fallback. */
	uint64_t	avr_cnthctl_el2;
	/* Affinity reported by a guest read of MPIDR_EL1. */
	uint64_t	avr_vmpidr_el2;
	/* Request a short host-timer deferral to guarantee guest progress. */
	uint64_t	avr_defer_cnthv;

	/* Host state overwritten while the guest context is installed. */
	uint64_t	avr_host_sp;
	uint64_t	avr_host_sp_el0;
	uint64_t	avr_host_pc;
	uint64_t	avr_host_pstate;
	uint64_t	avr_host_elr_el1;
	uint64_t	avr_host_spsr_el1;
	uint64_t	avr_host_esr_el1;
	uint64_t	avr_host_far_el1;
	uint64_t	avr_host_sctlr_el1;
	uint64_t	avr_host_tcr_el1;
	uint64_t	avr_host_ttbr0_el1;
	uint64_t	avr_host_ttbr1_el1;
	uint64_t	avr_host_mair_el1;
	uint64_t	avr_host_vbar_el1;
	uint64_t	avr_host_contextidr_el1;
	uint64_t	avr_host_cpacr_el1;
	uint64_t	avr_host_cntkctl_el1;
	uint64_t	avr_host_tpidr_el0;
	uint64_t	avr_host_tpidrro_el0;
	uint64_t	avr_host_tpidr_el1;
	uint64_t	avr_host_cntvoff_el2;
	uint64_t	avr_host_cnthctl_el2;
	/* Original VHE host virtual-timer state while it is deferred. */
	uint64_t	avr_host_cnthv_ctl_el2;
	uint64_t	avr_host_cnthv_cval_el2;
	uint64_t	avr_host_vmpidr_el2;
	uint64_t	avr_host_hcr_el2;
	/* GIC virtual-interface registers are physical-CPU EL2 state. */
	uint64_t	avr_host_ich_hcr_el2;
	uint64_t	avr_host_ich_vmcr_el2;
	uint64_t	avr_host_ich_lr0_el2;
	uint64_t	avr_host_ich_lr1_el2;
	/* Pointer-authentication keys are shared hardware state. */
	uint64_t	avr_host_pauth[10] __aligned(16);
};

struct vcpu {
	vaddr_t			vc_control_va;	/* [I] EL2 run page */
	paddr_t			vc_control_pa;	/* [I] */
	/* 16KB hardware stage 2; only one vCPU is currently supported. */
	struct arm64_vmm_s2_table *vc_s2_root;	/* [v] */
	struct arm64_vmm_s2_table *vc_s2_tables;	/* [v] */
	struct vm		*vc_parent;	/* [I] */
	uint32_t		vc_id;		/* [I] */
	u_int			vc_state;	/* [a] */
	SLIST_ENTRY(vcpu)	vc_vcpu_link;	/* [V] */
	uint8_t			vc_virt_mode;	/* [I] */
	struct rwlock		vc_lock;
	struct cpu_info		*vc_curcpu;	/* [a] */
	/* Physical CPU containing the newest resident guest EL12 bank. */
	struct cpu_info		*vc_lastcpu;	/* [v] */
	/* The saved EL1 bank differs from the last resident VHE EL12 bank. */
	uint8_t			vc_el12_dirty;	/* [v] */
	/* Encoded virtual IRQ identity, priority, and line level. */
	uint64_t		vc_intr;	/* [a] */
	/* GIC configuration for the kernel-originated architectural timer PPI. */
	uint64_t		vc_timer_intr;	/* [a] */
	/* Host-counter deadline and guest CTL sampled on the most recent exit. */
	uint64_t		vc_timer_deadline; /* [v] */
	uint64_t		vc_timer_ctl;	/* [v] */
	/* The last exit snapshot awaits completion by the next VMM_IOC_RUN. */
	uint8_t			vc_exit_pending;	/* [v] */
	/* A bounded async yield requests an early protected entry next RUN. */
	uint8_t			vc_entry_recovery;	/* [v] */
	struct vm_exit		vc_exit;	/* [v] */
	struct vcpu_reg_state	vc_regs;	/* [v] */
	struct vcpu_inject_event vc_inject;	/* [v] */
};

SLIST_HEAD(vcpu_head, vcpu);

extern int arm64_has_el2;

int	vmm_enabled(void);
int	vmm_probe_machdep(struct device *, void *, void *);
void	vmm_attach_machdep(struct device *, struct device *, void *);
void	vmm_activate_machdep(struct device *, int);
int	vmm_start(void);
int	vmm_stop(void);
int	pledge_ioctl_vmm_machdep(struct proc *, long);
int	vm_impl_init(struct vm *, struct proc *);
void	vm_impl_deinit(struct vm *);
int	vcpu_init(struct vcpu *, struct vm_create_params *);
void	vcpu_deinit(struct vcpu *);
int	vcpu_reset_regs(struct vcpu *, struct vcpu_reg_state *);
int	vm_rwregs(struct vm_rwregs_params *, int);

#endif /* _KERNEL */

#endif /* ! _MACHINE_VMMVAR_H_ */
