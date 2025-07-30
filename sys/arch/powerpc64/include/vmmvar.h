/*	$OpenBSD */
/*
 * Copyright (c) 2024 Mike Larkin <mlarkin@openbsd.org>
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

#define VMM_PCI_MMIO_BAR_BASE	0xF0000000ULL
#define VMM_PCI_MMIO_BAR_END	0xFFDFFFFFULL		/* 2 MiB below 4 GiB */

/* Exit Reasons */
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

struct vcpu_reg_state {
	uint64_t			vrs_gprs[VCPU_REGS_NGPRS];
};

enum {
	VMM_MODE_UNKNOWN,
	VMM_MODE_CLASSIC,
};

/*
 * struct vm_exit
 *
 * Contains VM exit information communicated to vmd(8). This information is
 * gathered by vmm(4) from the CPU on each exit that requires help from vmd.
 */
struct vm_exit {
	struct vcpu_reg_state		vrs;
};

struct vm_intr_params {
	/* Input parameters to VMM_IOC_INTR */
	uint32_t		vip_vm_id;
	uint32_t		vip_vcpu_id;
	uint16_t		vip_intr;
};

#define VM_RWREGS_GPRS	0x1	/* read/write GPRs */
#define VM_RWREGS_ALL	(VM_RWREGS_GPRS)

struct vm_rwregs_params {
	/*
	 * Input/output parameters to VMM_IOC_READREGS /
	 * VMM_IOC_WRITEREGS
	 */
	uint32_t		vrwp_vm_id;
	uint32_t		vrwp_vcpu_id;
	uint64_t		vrwp_mask;
	struct vcpu_reg_state	vrwp_regs;
};

/* IOCTL definitions */
#define VMM_IOC_INTR _IOW('V', 6, struct vm_intr_params) /* Intr pending */

/*
 * Virtual CPU
 *
 * Methods used to vcpu struct members:
 *	a	atomic operations
 *	I	immutable operations
 *	K	kernel lock
 *	r	reference count
 *	v	vcpu rwlock
 *	V	vm struct's vcpu list lock (vm_vcpu_lock)
 */
struct vcpu {
	struct vm *vc_parent;			/* [I] */
	uint32_t vc_id;				/* [I] */
	u_int vc_state;				/* [a] */
	SLIST_ENTRY(vcpu) vc_vcpu_link;		/* [V] */

	struct rwlock vc_lock;
};

SLIST_HEAD(vcpu_head, vcpu);

/* Forward declarations */
struct vm;
struct vm_create_params;

int	vmm_start(void);
int	vmm_stop(void);
int	vm_impl_init(struct vm *, struct proc *);
void	vm_impl_deinit(struct vm *);
int	vcpu_init(struct vcpu *, struct vm_create_params *);
void	vcpu_deinit(struct vcpu *);
void	vmm_attach_machdep(struct device *, struct device *, void *);
void	vmm_activate_machdep(struct device *, int);
int	vm_rwregs(struct vm_rwregs_params *, int);
int	pledge_ioctl_vmm_machdep(struct proc *, long);
int	vcpu_reset_regs(struct vcpu *, struct vcpu_reg_state *);


#endif /* ! _MACHINE_VMMVAR_H_ */
