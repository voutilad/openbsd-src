# Controlled EL0-entry invalidation test, 2026-09-28

## Question and predictions (recorded before running)

Does additional translation invalidation release the ramdisk init stall,
or did the earlier experiment help merely through its barriers/timing?

Use one 512MiB bsd.rd VM at a time under the 6GiB OpenBSD QEMU VM. Keep
the payload, timer injection, normal mandatory invalidations, and normal
register save/restore paths unchanged. Do not pause during measurement.

Payload: `/home/dv/bsd.rd.vmm`, RAMDISK#8, SHA256
`1ca8952089f52a4533294245d5965fdd9aa5399ed840aada0c5f4a066e91e1d7`.
The counter offsets in vmm-inspect.c were checked with nm for this image.
They must be updated for a different ramdisk kernel; this is not a generic
guest introspection tool.

Six 15-second phases start at the first observed return to EL0:

| Phase | Forward run | Reverse run |
| --- | --- | --- |
| 0 | No extra operation | No extra operation |
| 1 | DSB ISHST; DSB ISH; ISB only | Same barriers |
| 2 | Barriers around VMALLE1IS | Barriers around VMALLS12E1IS |
| 3 | No extra operation | No extra operation |
| 4 | Barriers around VMALLS12E1IS | Barriers around VMALLE1IS |
| 5 | No extra operation | No extra operation |

The additional operation is requested only when the saved guest PSTATE is
EL0. Invalidation uses the existing guest VMID with TGE clear. Required
stage-2/migration invalidations still execute independently in every mode.

Read guest uvmexp trap/interrupt/syscall/context-switch counters, ticks,
and uptime with SHAREMEM every five seconds. The helper writes no guest
RAM and makes no READREGS request during these phases. Kernel telemetry
records guest PC, exits, S2 mapping counts, and phase/mode every five seconds.
After phase 5, pause only to collect registers and a software page-table walk.

- Translation hypothesis supported: persistent stall during both controls,
  followed by reproducible instruction/syscall progress in TLBI phases.
- Stage-1 invalidation sufficient: VMALLE1IS releases the original stall
  without requiring VMALLS12E1IS.
- Barriers sufficient: barriers-only releases it; invalidation not isolated.
- Repeated guest fault hypothesis: guest trap counter rises rapidly while
  syscalls and PC make no progress. Saved ESR alone is not this evidence.

Limitations: TLBI can cause different outer-KVM work/latency than barriers;
progress under TLBI alone does not locate the defect in OpenBSD versus KVM.
The forward/reverse runs also have different randomized guest address-space
state. This is a diagnostic experiment, not a proposed permanent workaround.

## Results

Forward run completed on GENERIC.MP#177, kernel VM id 1. Raw evidence:
`vmm-tlb-forward.8aax9b12/{samples,dmesg,registers-and-walk,console}`.

| Observed interval (UTC) | Operation | Guest progress |
| --- | --- | --- |
| 19:43:21–19:43:31 | Baseline | PC 0x28dee0; traps 6; syscalls 0 |
| 19:43:36–19:43:46 | Barriers only | Same PC, traps and syscalls |
| 19:43:51–19:44:02 | VMALLE1IS | Same PC, traps and syscalls |
| 19:44:07–19:44:12 | Baseline | Still unchanged |
| 19:44:22–19:44:27 | VMALLS12E1IS | PC advances; traps 65→83; syscalls 8→16 |
| 19:44:32–19:44:47 | Baseline again | PC 0x2f4024; traps 83; syscalls 16 |

Guest interrupt and context-switch counts continued increasing in stalled
phases: for example interrupts 43→244 and switches 107→550 during the first
baseline/barrier/S1/baseline sequence. Thus the saved instruction-abort
syndrome was NOT evidence of a repeating instruction-fault loop. Guest
kernel interrupt handling still executes while EL0 makes no progress.

Counter reads and the most recent five-second kernel log are not atomic.
The 19:44:17 sample already has five syscalls, while the last kernel log
still labels phase 3. Treat transition-boundary samples as ambiguous; the
interior samples above do not depend on that label lag.

After measurement, the page-table walk for 0x2f4024 found a valid executable
leaf, PTE 0x2000005e06ef8f. Saved ESR_EL1 is 0x82000007, again not proof of
an ongoing fault. No installer prompt was reached.

The first reverse attempt (`vmm-tlb-reverse.kuqrS7gF`, VM id 2) was aborted
by a harness bug: it used old VM-id-1 phase-5 log lines before its new VM
started. It is EXCLUDED. The driver now filters each telemetry block by
kernel VM id. Replacement reverse run is VM id 3.

Reverse run completed on the same GENERIC.MP#177, kernel VM id 3. Evidence:
`vmm-tlb-reverse.xoZXYoOz/{samples,dmesg,registers-and-walk,console}`.

| Observed interval (UTC) | Operation | Guest progress |
| --- | --- | --- |
| 19:48:47–19:48:57 | Baseline | PC 0x28dee0; traps 6; syscalls 0 |
| 19:49:02–19:49:08 | Barriers only | Same PC, traps and syscalls |
| 19:49:18–19:49:28 | VMALLS12E1IS | PC advances; traps 72→299; syscalls 11→82 |
| 19:49:33–19:49:43 | Baseline again | PC 0x40c5b0; traps 299; syscalls 82 |
| 19:49:48–19:49:58 | VMALLE1IS | Same stalled PC, traps and syscalls |
| 19:50:03–19:50:18 | Baseline again | Still unchanged |

Interrupts continued (215→397) across the final baseline/S1/baseline phases.
Final PTE for PC 0x40c5b0 was 0x2000005e038f8f, valid and executable.
This VM migrated from outer CPU 2 to CPU 1 during the combined phase, but
already had 41 syscalls before the migration. The forward run remained on
CPU 2 through all measured phases, independently excluding migration as
necessary for the observed release.

Both runs support the same result: barriers and stage-1-only invalidation
do not release the stall; combined invalidation permits real userspace
progress, which stops again when the extra combined operation stops.
Neither reaches the installer prompt in these short controlled intervals.
No instruction emulation was added to vmm.

This rejects the repeated-guest-instruction-fault interpretation and weakens
the simple stage-1/ASID-invalidation hypothesis. It does NOT identify the
precise root cause. Remaining discriminators include instruction-cache-only
maintenance versus stage-2 invalidation, and inspecting the outer KVM's
shadow mapping/refill path. A combined TLBI is not an isolated test of CPU
TLB contents under nesting: upstream KVM's handle_vmalls12e1is unmaps the
matching nested shadow S2 ranges (the exact host build still needs checking):
https://github.com/torvalds/linux/blob/master/arch/arm64/kvm/sys_regs.c

Host swap remained zero (about 6.9GiB available); outer OpenBSD also reported
zero swap pages. No new host kernel warnings appeared during the tests.

## Reproduction and scope

A dedicated got branch reference `vmm-arm64-tlb-ab` was created for the
temporary instrumentation/tools. COMMIT IS PENDING: after both experiments
completed, the outer VM became unresponsive during the full-tree
`got update -b vmm-arm64-tlb-ab`. The reference was created from 9b69f44b,
but the worktree switch has not returned and no experiment commit exists.
Local hwfast-work copies preserve both kernel files, all tools, and both
completed runs' evidence. This is NOT a production fix. Do not merge the
experiment's automatic timed invalidations into normal vmm operation.

Build/install the experimental kernel and reboot the outer VM. Copy
vmm-inspect.c, bsd-rd-test.sh, and tlb-ab-test.sh to /tmp inside that VM.
Compile the reader with:

    cc -Wall -Wextra -I/usr/src/sys -o /tmp/vmm-inspect /tmp/vmm-inspect.c

With no other VMs created since that reboot, run sequentially:

    doas sh /tmp/tlb-ab-test.sh 1 forward
    doas sh /tmp/tlb-ab-test.sh 2 reverse

The numeric arguments are KERNEL VM ids, not vmctl's display id. A failed
creation may consume an id. This session used 1 and 3 because of the
excluded failed attempt described above. Do not run other VMs concurrently:
the temporary telemetry/phase state supports only one VM at a time.

The driver creates a private /home/dv/vmm-tlb-* result directory, performs
read-only counter sampling, then pauses only AFTER the measured phases to
capture registers/page tables. It shuts down the nested VM and its vmd.
Scripts assume the dedicated development VM and the exact ramdisk payload
above, not an arbitrary production host.

## Cleanup blocked, 19:58 UTC

Both nested test VMs and their vmd processes were cleaned up successfully
before loss of outer-VM responsiveness. The active outer kernel remains
GENERIC.MP#177 with experimental instrumentation; restoration of the normal
kernel is NOT complete. The got branch-switch SSH session is still pending.

QEMU monitor initially answered `info status` (running) and CPU-0 register
inspection: PC=ffffff800075fe34, x30=ffffff800075fe10, PSTATE=814003c9.
`cpu 1; info registers` did not complete. SSH times out during banner
exchange. Serial briefly produced a login prompt but did not process a
subsequent login normally. No force-reset or power-off was performed.
Linux swap is still zero, about 6.8GiB available, and its kernel journal
has no new warnings. The cause of this separate outer-VM stall is unknown.

## Recovery, Sep 29 00:08 UTC

The existing got update and QEMU register-query handles both completed when
rechecked, and SSH answered again. No restart was used. The worktree is now
on vmm-arm64-tlb-ab; the two modified kernel files and four diagnostic files
are present. The earlier blocked-cleanup account above records the observed
state at that time, not proof the process had terminated. The user has now
authorized force-resetting the outer VM when required for further work.
