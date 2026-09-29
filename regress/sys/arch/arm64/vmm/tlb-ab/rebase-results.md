# arm64 vmm/vmd rebase onto September 29 master

The work is on `vmm-arm64-rebase`, based on master
`2fa008c689040ab342c857045f47c45b8297cc8d`. The original `vmm-arm64`
and `vmm-arm64-tlb-ab` branches were preserved. `got rebase` replayed the
complete implementation, including the actual 16KB stage-2 tables.

## Compatibility changes

Current vmm assigns a file descriptor to each VM. Only CREATE uses
`/dev/vmm`; RUN, register access, interrupt configuration/injection and
SHAREMEM use the returned descriptor. Closing its last reference replaces
the old termination ioctl. The arm64 handlers now receive the VM reference
held by the common ioctl dispatcher, rather than looking up numeric IDs.

The port also supplies the arm64 termination IPI, preserves termination
requests at RUN exit, allows IRQCFG through the pledged VM descriptor,
and uses vmd's current interruptible-halt interface. A kick-only interrupt
operation leaves an asserted device IRQ unchanged during pause/shutdown.
The regressions exercise inherited/duplicated descriptors, two independent
VMs, pledged ioctls, and a queued IRQ surviving a kick.

No guest instruction emulation was added to the kernel. Guest GIC CPU
interface, virtual timer and stage-1 TLB maintenance remain hardware paths;
Distributor/Redistributor and PL011 device emulation remain in vmd.

## Timer race found during verification

The first current ramdisk boot passed all shell commands. A repeat then
reproduced the historical stall after echoing `p` from `pwd`.

Inspection found that `arm64_timer_schedule_locked()` called the software
timer interrupt path if the deadline expired during the handoff to the
event thread. In hardware mode this incorrectly published timer INTID 27
as a userland device interrupt, even though the kernel already supplies
that timer in its separate LR. The deterministic `timer_schedule` test
failed on this path before the fix. It now passes: an overdue deadline
queues an immediate callback, which wakes the hardware-timer vCPU without
asserting a second software GIC line. The software fallback and masked
hardware-timer cases are covered too.

## Verified build and runtime

The clean two-job GENERIC.MP build and matching vmd/vmctl builds passed.
The running outer kernel is `GENERIC.MP#184` with
`vmm0 at mainbus0: EL2/VHE stage-2 (16KB)`. Installed userland binaries were
compared with stripped copies of the build outputs.

All nine `regress/sys/arch/arm64/vmm` targets passed, including the 18 TLBI
cases. The userland suite passed the timer-handoff unit test, FDT/GIC SPI,
hardware timer PPI, repeated timer wakeups, timer preemption of an active
SPI, and PL011 TX, RX and burst-input checks.

The current `/bsd.rd` is OpenBSD 8.0 RAMDISK#101, built September 29:

    6407834e4dc79c50ab559ed275c1da346264d773b73f7b14080c5437b37a4c4b

Each shell check uses one vCPU, 512MB RAM, no disk and no network. It selects
S, then checks echo, pwd, ls /, sysctl, shell arithmetic and sleep. Markers
are split across shell quotes so echoed input cannot falsely pass a check.
The test can be repeated with no running vmd instance:

    doas env BSD_RD=/bsd.rd sh \
        /usr/src/regress/sys/arch/arm64/vmm/tlb-ab/bsd-rd-shell-test.sh

After the timer fix, three consecutive boots passed every shell check,
without restarting the outer VM or pausing/resuming a nested vCPU:

- `/home/dv/bsd-rd-shell.cqk3V4wy`
- `/home/dv/bsd-rd-shell.XJvNGfUJ`
- `/home/dv/bsd-rd-shell.NbbhEjGW`

Each directory contains a `BSD_RD_SHELL_PASS` result, payload hash, outer
kernel identity, timestamps, vmd log and complete console capture. The third
console capture is also committed as `rebase-shell-console.log`. These runs
establish the requested installer-shell milestone, not exhaustive stability
under arbitrary guest workloads. All test VMs and their vmd instances were
stopped by the harness; the outer OpenBSD VM remains running.

## Limits and observations

This remains the initial UP platform, not an SMP, disk or networking port.
The first verification sequence encountered an outer-VM-wide slowdown
while compiling the toy guest, before a nested VM was launched. It
eventually progressed; a clean powerdown/restart was used before ramdisk
verification. Its cause was not established and is not claimed fixed.
Host swap remained zero throughout, with the outer VM still limited to 6GB.
