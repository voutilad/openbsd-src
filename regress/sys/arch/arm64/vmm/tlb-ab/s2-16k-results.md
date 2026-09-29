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

## Clean-kernel confirmation

GENERIC.MP#182 boots commit 9d79f356 and reports `EL2/VHE stage-2 (16KB)`.
All eight kernel regression targets and the vmd device regression passed
again after reboot, with no diagnostic tracing or extra invalidation code
remaining in the kernel.

A first clean-kernel boot reached the installer but timed out (60 seconds)
after echoing the `i` of its initial `i\r` input. It did not print the next
question. Preserve this as an unresolved intermittent input symptom, not
as a successful automated run. Evidence: bsd-rd-s2-clean-first-{result,console}.

The next clean-kernel boot, bsd-rd-hwfast-74154, completed the entire harness
at 01:23:06 UTC on Sep 29 and returned exit status 0 / BSD_RD_BOOT_PASS.
It accepted all installer answers through the no-disks prompt. This repeat
allowed up to 180 seconds per prompt, but actually completed at about the
same overall boot duration as the original successful #180 run. No input
was retransmitted, no VM pause occurred, and no execution state was changed.
Evidence: bsd-rd-s2-clean-repeat-{result,console,vmd.log}. Copies are also
in the Linux workspace's hwfast-work directory.

Commit fe82fcbd extends the toy UART regression beyond its original single
character. It sends `i\rhostname\rpassword\r` in one write and checks every
received character in order across repeated RX interrupts and rearming.
The first run passed, followed by two successful stress repetitions. Stress
trial 3 timed out before printing any toy-guest output, before input was
sent. A further run with UART_WAIT_LIMIT=600 (nominal 60 seconds waiting
for the input-ready marker) also failed: it printed successful SPI, timer,
repeated-wakeup, and timer-preemption messages, but never printed the UART
TX completion message. It ended at 01:28:20 UTC after starting at 01:27:06.
Thus this is not established to be a UART byte-loss issue or merely the
original short startup timeout. Do NOT describe the stress suite as passing.

Evidence: vmd-s2-burst-regress.log, vmd-s2-burst-{1,2,3}.log, and
vmd-s2-burst-long.log. All are retained on both hosts. UART_WAIT_LIMIT is an
optional test-harness bound, not a device workaround. No kernel or vmd
behavior was changed to hide these failures.

The installer-boot milestone is achieved, but repeated-start/interrupt/input
reliability remains unfinished. The next investigation should capture the
toy guest's stopped PC and LR/VMCR/PMR state during the pre-UART-TX stall;
it is a much smaller case than the ramdisk. The production kernel has no
diagnostic tracing, and all temporary nested test guests are stopped.

## Manual reproduction inside the outer OpenBSD VM

Run vmd in one terminal:

    doas vmd -d -f /dev/null

In another terminal, launch and attach to the ramdisk (no disks or NICs):

    doas vmctl start -c -m 512M -b /home/dv/bsd.rd.vmm rdtest

For the automated console-input check, with no other vmd instance running:

    doas env BOOT_LIMIT=1500 sh /usr/src/regress/sys/arch/arm64/vmm/tlb-ab/bsd-rd-test.sh

This harness stops its nested VM and vmd when done. The 512MB nested VM is
inside the existing 6GB QEMU/OpenBSD VM; do not start a second outer QEMU.
