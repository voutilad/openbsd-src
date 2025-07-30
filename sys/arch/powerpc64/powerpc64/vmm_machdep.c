/*	$OpenBSD */
/*
 * Copyright (c) 2014-2024 Mike Larkin <mlarkin@openbsd.org>
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
#include <sys/device.h>
#include <sys/proc.h>
#include <sys/types.h>

#include <uvm/uvm_extern.h>

#include <machine/vmmvar.h>

#include <dev/vmm/vmm.h>

#define DEVNAME(s)  ((s)->sc_dev.dv_xname)

void vmm_activate_machdep(struct device *, int);
int vmmioctl_machdep(dev_t, u_long, caddr_t, int, struct proc *);
int vmm_quiesce_vmx(void);
int vm_rwregs(struct vm_rwregs_params *, int);
int vm_rwvmparams(struct vm_rwvmparams_params *, int);
int vcpu_reset_regs(struct vcpu *, struct vcpu_reg_state *);
int vcpu_init(struct vcpu *, struct vm_create_params *);
void vcpu_deinit(struct vcpu *);

extern struct cfdriver vmm_cd;
extern const struct cfattach vmm_ca;

extern struct vmm_softc *vmm_softc;

void
vmm_attach_machdep(struct device *parent, struct device *self, void *aux)
{
}

void
vmm_activate_machdep(struct device *self, int act)
{
}

int
vmmioctl_machdep(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return (EINVAL);
}

int
pledge_ioctl_vmm_machdep(struct proc *p, long com)
{
	return (EPERM);
}

/*
 * vm_rwvmparams
 *
 * IOCTL handler to read/write the current vmm params like pvclock gpa, pvclock
 * version, etc.
 *
 * Parameters:
 *   vrwp: Describes the VM and VCPU to get/set the params from
 *   dir: 0 for reading, 1 for writing
 *
 * Return values:
 *  0: if successful
 *  ENOENT: if the VM/VCPU defined by 'vpp' cannot be found
 *  EINVAL: if an error occurred reading the registers of the guest
 */
int
vm_rwvmparams(struct vm_rwvmparams_params *vpp, int dir)
{
	struct vm *vm;
	struct vcpu *vcpu;
	int error, ret = 0;

	/* Find the desired VM */
	error = vm_find(vpp->vpp_vm_id, &vm);

	/* Not found? exit. */
	if (error != 0)
		return (error);

	vcpu = vm_find_vcpu(vm, vpp->vpp_vcpu_id);

	if (vcpu == NULL) {
		ret = ENOENT;
		goto out;
	}

out:
	refcnt_rele_wake(&vm->vm_refcnt);
	return (ret);
}

/*
 * vmm_start
 *
 * Starts VMM mode on the system
 */
int
vmm_start(void)
{
	int rv = 0;

	return (rv);
}

/*
 * vmm_stop
 *
 * Stops VMM mode on the system
 */
int
vmm_stop(void)
{
	int rv = 0;

	return (rv);
}

int
vm_impl_init(struct vm *vm, struct proc *p)
{
	return (0);
}

void
vm_impl_deinit(struct vm *vm)
{
	/* unused */
}

/*
 * vcpu_reset_regs
 *
 * Resets a vcpu's registers to the provided state
 *
 * Parameters:
 *  vcpu: the vcpu whose registers shall be reset
 *  vrs: the desired register state
 *
 * Return values:
 *  0: the vcpu's registers were successfully reset
 *  !0: the vcpu's registers could not be reset (see arch-specific reset
 *      function for various values that can be returned here)
 */
int
vcpu_reset_regs(struct vcpu *vcpu, struct vcpu_reg_state *vrs)
{
	return (0);
}

/*
 * vcpu_init
 *
 * Calls the architecture-specific VCPU init routine
 */
int
vcpu_init(struct vcpu *vcpu, struct vm_create_params *vcp)
{
	int ret = 0;

	return (ret);
}

/*
 * vcpu_deinit
 *
 * Calls the architecture-specific VCPU deinit routine
 *
 * Parameters:
 *  vcpu: the vcpu to be deinited
 */
void
vcpu_deinit(struct vcpu *vcpu)
{
}

/*
 * vm_readregs
 *
 * IOCTL handler to read/write the current register values of a guest VCPU.
 * The VCPU must not be running.
 *
 * Parameters:
 *   vrwp: Describes the VM and VCPU to get/set the registers from. The
 *    register values are returned here as well.
 *   dir: 0 for reading, 1 for writing
 *
 * Return values:
 *  0: if successful
 *  ENOENT: if the VM/VCPU defined by 'vrwp' cannot be found
 *  EINVAL: if an error occurred accessing the registers of the guest
 *  EPERM: if the vm cannot be accessed from the calling process
 */
int
vm_rwregs(struct vm_rwregs_params *vrwp, int dir)
{
	return (ENOENT);
}

int
vm_run(struct vm_run_params *vrp)
{
	return (ENOENT);
}

