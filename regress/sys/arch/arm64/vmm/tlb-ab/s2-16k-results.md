# Actual 16KB stage-2 tables resolve the nested execute stall

## Controlled test

The backing allocator already supplied aligned, physically contiguous 16KB
runs, but VTCR_EL2.TG0 remained 4KB and VTTBR_EL2 pointed at the native 4KB
pmap. Alignment alone did not select 16KB hardware translation tables.

The tiny tlbi regression in commit 426c0d11 contains these controls:

| Case | 4KB stage 2 (#179) | 16KB stage 2 (#180) |
| --- | --- | --- |
| Original 16 stage-1 TLBI cases | pass | pass |
| Execute code in a fresh, separate 16KB region | pass | pass |
| Read that region as data, then execute it | timeout | pass |

In the failing case, exactly one guest translation fault was handled, then
PC and ELR remained 0x300000. Guest interrupts were masked, the timer was
disabled, and no virtual interrupt was pending. The failure therefore did
not require OpenBSD userland, a GIC, or timer delivery.

The successful kernel changed the actual stage-2 granule and root: 16KB
tables, eleven index bits per full level, VTCR.TG0=2 and SL0=2 (L1), with
the same 39-bit IPA space. Both OpenBSD kernels retain 4KB stage-1 pages.
Native pmap entries still account for each constituent 4KB backing page and
perform cache maintenance. The hardware tables map each aligned 16KB run.

## Ramdisk result

GENERIC.MP#180, commit ccf107ef, booted /home/dv/bsd.rd.vmm through ordinary
vmd startup with 512MB RAM. The name was bsd-rd-hwfast-89510, NOT a diagnostic
bsd-rd-tlb-* name. No forced entry invalidation was enabled.

The console reached the installer, accepted answers, and printed:

    Available disks are: none.
    Which disk is the root disk? ('?' for details)

The first harness incorrectly expected a timezone question before disk
selection and consequently reported a timeout AFTER this successful boot.
The console is preserved in /home/dv/bsd-rd-s2-16k-console. The harness has
been corrected to accept either question order; no disks are attached or
written. A clean-kernel confirmation run follows below.

All eight vmm kernel regression targets passed on #180: vcpu, preempt,
inject, sysreg, timer, fp, pauth, tlbi. The vmd FDT/GIC/UART toy-guest
regression also passed. Logs: /home/dv/vmm-s2-16k-regress.log and
/home/dv/vmd-s2-16k-regress.log. Linux host swap remained zero; outer RAM
was still 6GB. No host configuration or kernel change was needed.

## Interpretation and implementation boundaries

The reduced test and successful ramdisk boot establish the granule change
as an effective fix on this nested host. A likely detailed KVM mechanism is
that a 4KB nested mapping size differs from the host's 16KB permission-fault
granule, selecting the mapping path instead of the permission-relaxation
path. The mapping path refuses a permissions-only replacement with EAGAIN,
expecting a later fault to use relaxation. That can explain why removing
shadow mappings temporarily helped but an ordinary S1 TLBI did not.

This detailed Linux path is inferred from upstream code, NOT traced in the
running Fedora kernel. Relevant primary sources:

- https://github.com/torvalds/linux/blob/master/arch/arm64/kvm/mmu.c
  (kvm_s2_fault_map and its mapping_size/perm_fault_granule comparison)
- https://github.com/torvalds/linux/blob/master/arch/arm64/kvm/hyp/pgtable.c
  (stage2_map_walker_try_leaf and stage2_pte_needs_update)
- https://github.com/torvalds/linux/blob/master/arch/arm64/include/asm/kvm_arm.h
  (granule-dependent SL0 encoding)

The initial independent hardware table tree is deliberately UP-only, like
the existing vmm implementation. Before adding SMP, move its ownership and
locking to VM-wide state. It is not yet a general multi-granule pmap design.
No guest instruction decoding/emulation was added to the kernel.

Temporary trace output and all name-controlled forced-flush modes are
removed in the follow-up kernel; historical diagnostic drivers in tlb-ab
require the older diagnostic commits and are retained only as evidence.
