# Instruction-cache-only discriminator, Sep 29

Question: does the combined-TLBI experiment release init because instruction
cache contents become visible, rather than because translations change?

GENERIC.MP#178 adds mode 4, IC IALLUIS, with the SAME DSB ISHST / DSB ISH /
ISB sequence as the existing controls. It does not execute a TLBI in this
additional mode. Required mapping/migration invalidations remain unchanged.
Only diagnostic VM names starting bsd-rd-tlb- enable forced operations.

Run `doas sh /tmp/tlb-ab-test.sh 1 icache` after a fresh outer boot. Six
15-second phases after first observed EL0 entry are baseline, barriers,
IC IALLUIS, baseline, VMALLS12E1IS, baseline. Same ramdisk and 512MiB RAM as
the earlier experiment; no pause until all timed measurements finish.

Predictions:

- IC-only progress, while barriers fail: supports instruction-cache
  visibility as the missing effect. It still needs a smaller reproducer
  and a specific maintenance/context error before becoming a fix.
- IC-only fails but combined succeeds: shifts attention to nested shadow
  S2 state or another combined-invalidation side effect. Does not by itself
  prove which implementation is wrong.

Result: completed. Evidence directory /home/dv/vmm-tlb-icache.JNeGa4tF,
also copied to the local hwfast-work directory.

The guest reached 0x413f54 before the IC-only phase. At 00:48:59–00:49:39
UTC, barriers, IC-only, and the following baseline all had traps=8 and
syscalls=0, with PC fixed at 0x413f54. Interrupts rose from 70 to 231 and
context switches from 167 to 521, so kernel interrupt handling still ran.

Combined invalidation at 00:49:44–00:49:54 advanced through multiple user
PCs and reached at least 50 syscalls in phase-4 interior samples. At the
first phase-5 sample it had reached 86 syscalls and 312 traps. Those counts
then stayed fixed at PC 0x33fd84 through the final four samples, while
interrupts continued. No installer prompt was reached in this short run.

Thus IC IALLUIS alone is insufficient. This narrows but does not completely
exclude cache visibility (e.g. data-cache cleaning is a different operation).
The next positive-control boot keeps combined invalidation enabled until
installer output or a bounded timeout, instead of stopping after 15 seconds.

The initial build attempt was interrupted by a severe outer-VM slowdown.
The user authorized forced resets. QEMU monitor reset could not complete;
SIGKILL left one vCPU thread temporarily running with the kill signal pending,
but it eventually exited. The original handle was confirmed finished before
starting a replacement QEMU. No Linux host reboot occurred. Automatic fsck
repaired filesystem inconsistencies. Two edited source files did not match
the local intended versions and were recopied and SHA256-verified before
rebuilding. The subsequent build/install/reboot completed normally. Keep
these recovery events distinct from the measured nested-guest stall.

The first experiment is committed as 0b40676b on vmm-arm64-tlb-ab. The
IC-only extension and this result are recorded in a following commit on
that same diagnostic branch.

## Continuous combined-invalidation control and reduced reproducer

GENERIC.MP#179 kept VMALLS12E1IS enabled at every EL0 re-entry for the
diagnostic name bsd-rd-tlb-continuous. It reached 211 syscalls and 821 traps,
then remained at PC 0x409ea3000 for over a minute. A paused page-table walk
found a valid executable PTE mapping IPA 0x5e100000 and instruction d10883ff,
the first instruction of the kernel-provided signal trampoline. X0 was 20
(SIGCHLD). Thus repeated combined invalidation is not a sufficient fix.
Evidence: /home/dv/vmm-tlb-positive.qVhEoiOT (also copied to the host).

The tlbi regression now adds two controls using a code target in a separate
16KB physical region (IPA 0x10000). All original 16 cases pass, and executing
the separate region without touching it as data passes. Reading the region
as data before executing it fails: one guest translation fault was handled,
then PC and ELR stay at 0x300000. Guest interrupts are masked, the virtual
timer is disabled, and both virtual interrupt list registers are empty.
This isolates data-to-executable permission promotion from the ramdisk,
timer, and GIC. The regression is committed as 426c0d11.

Next discriminator: the backing allocation is 16KB aligned/contiguous, but
VTCR.TG0 is still 4KB and the native pmap uses 4KB hardware tables. Test
actual 16KB stage-2 translation tables against the small reproducer before
trying another ramdisk boot. A nested KVM granule/permission interaction is
a hypothesis, not yet a proven root cause.
