# Initial arm64 PCI virtio platform

The unmodified arm64 RAMDISK kernel already includes pciecam, virtio PCI,
vioblk and vio. No guest driver, firmware or kernel instruction-emulation
changes are needed for this platform. vmd directly loads the ELF kernel
and supplies the FDT as before.

## Discovery and register access

The FDT describes one `pci-host-ecam-generic` bridge, bus 0 only. It does
not describe individual endpoints; the guest enumerates those through PCI.

* ECAM: 0x10000000, size 1MB. Offsets encode slot in bits 19:15, function
  in bits 14:12 and register in bits 11:0. Only function 0 is populated.
* PCI memory: 0x20000000, size 256MB, with identity bus/CPU translation.
  The current allocator gives each BAR 64KB. BAR probes and relocation
  are supported within the platform aperture; memory decoding obeys the
  PCI command register. Both windows are below RAM at 0x40000000.
* All windows are unbacked MMIO ranges aligned to the 16KB stage-2 granule.
  Kernel exits carry the access syndrome; vmd completes each register
  access and advances the guest PC. There is no kernel instruction decoder.
* Endpoints implement the conventional 256-byte PCI header. Other ECAM
  offsets do not alias this header. Missing functions read as all ones;
  implemented functions have no extended capabilities.

Virtio 1.x capabilities point at memory BARs on arm64, and continue to point
at I/O BARs on amd64. The arm64 adapter converts MMIO direction/width into
the existing register-handler interface. Native 64-bit accesses become two
ordered 32-bit accesses. Block and network still run in their existing
separate, pledged device processes; guest RAM is shared via SHAREMEM.
The existing modern entropy device uses the same transport. The legacy
VMM control interface and CD-ROM are not exposed on arm64 in this increment.

## Interrupts

There is no MSI/MSI-X capability or FDT MSI controller on arm64 yet. The
unmodified guest takes its existing INTx path. A slot's INTA maps to
`40 + (slot % 4)`; the FDT also describes the standard swizzle for the other
three pins. Those four SPIs fit in the current GIC's 64-INTID space.

Each endpoint retains its asserted state. PCI routing takes the wired-OR
of unmasked endpoints sharing a line: reading one virtio ISR must not clear
another device's interrupt. PCI interrupt-disable masks delivery without
discarding the endpoint's pending condition. IRQ changes go through the
existing userland GIC and VMM_IOC_INTR, not the x86 PIC or LAPIC.

The existing kernel backend still has one device List Register and a separate
timer List Register. This is an initial UP implementation, not an extension
to SMP, ITS/LPIs, MSI-X or multiqueue. Multi-source operation is exercised by
running disk reads and network traffic while the console and timer are live.

The accelerated CPU interface must retain more than its List Registers.
Acknowledging an interrupt also sets an active-priority bit, which is dropped
by EOIR. The EL2 context switch now saves/restores both ICH_AP0R and ICH_AP1R
banks, including host state and the short asynchronous retry path. It accesses
only the registers implemented according to ICH_VTR_EL2.PREbits, restores
Group 0 before Group 1, and clears the saved guest banks on RESETCPU. This is
register context switching, not instruction emulation.

The `gicstate` regression makes VM A acknowledge an interrupt at priority
0x80, switches to VM B while A remains active, resumes A, and checks both EOI
and reset. Before the fix, B's ICC_RPR_EL1 incorrectly read 0x80 instead of
the idle priority 0xff. It now passes 32 switches and resets. The old kernel
failed this isolation test; use the matching fixed kernel before running it.
The prior kernel is preserved at `/bsd.vmm-pci-before-apr` in this development
VM. This fixes the demonstrated priority leak, not every nested-host stall.

## Shared queues

DMA addresses are guest physical addresses into RAM, not host addresses or
PCI BAR addresses. Existing range checks translate descriptors and buffers
to shared mappings. The 16KB stage-2 granule does not change virtio's own
descriptor/available/used alignment requirements.

Available-index loads acquire the guest's published descriptor writes; used
index stores release data/status/completion writes. Network completion
publication is followed by a store/load barrier before checking interrupt
suppression. This makes ordering explicit on arm64 instead of relying on
x86 load ordering. The publication regression crosses the 16-bit index wrap.

## Manual use inside the outer OpenBSD VM

With the matching kernel booted and vmd installed, start the daemon if one
is not already running (`doas vmd -f /dev/null`). Then create a NEW disposable
image (choose another filename if this one already exists):

```
vmctl create -s 1G /home/dv/pci-test.raw
doas vmctl start -c -m 512M -b /bsd.rd -d /home/dv/pci-test.raw -i 1 pci-test
```

The expected attachment chain is `pci0 at pciecam0`, then `virtio` PCI
endpoints with `vio0`, `vioblk0` and `sd0`. Choose S for the installer shell.
The shell may need `(cd /dev && sh MAKEDEV sd0)` before raw disk access.
`-i 1` creates an isolated TAP; it does not automatically connect the guest
to the Internet. Use a vmd switch or local interface configuration when
external connectivity is wanted.

## Regression commands

```
cd /usr/src/regress/usr.sbin/vmd/arm64
doas make regress
doas sh pci-boot.sh

cd /usr/src/regress/sys/arch/arm64/vmm
doas make regress
```

The boot test requires exclusive use of vmd and no existing TAP interfaces.
It creates a new 64MB image and a 512MB VM, checks PCI attachment, reads a
random sector by SHA-256, checks a disk write/read round trip, and exchanges
ICMP packets over an isolated 198.18.0.0/30 TAP network. It also overlaps
disk reads with network traffic. Commands are paced by completion markers
because flooding the current UART can overflow the guest's input buffer.
Cleanup stops only the test VM/daemon. Evidence and the disposable disk are
retained under the printed RESULT_DIRECTORY; no user disks are attached.

The two PCI/queue checks also run as unprivileged unit tests:
`./pci_ecam` checks configuration space, BARs and shared INTx routing;
`./virtqueue_order` checks payload publication across the 16-bit index wrap.

## Verification and remaining stability issue

With GENERIC.MP#185, all ten kernel vmm regression targets and the complete
arm64 userland suite passed. Two back-to-back ramdisk runs passed PCI
attachment, installer shell access, sector SHA-256 verification, disk write
and readback, and overlapping disk reads with ten successful network pings:

* `/home/dv/bsd-rd-pci.WyLZHYtW`
* `/home/dv/bsd-rd-pci.xcCabP2p`

The third run, `/home/dv/bsd-rd-pci.2hyq8CdV`, stalled before any console
output. Ordinary outer-VM programs, including the debugger, also stopped
making progress. QEMU sampled the nested CPU at PC 0x4020003c, very early in
kernel entry, before PCI enumeration. A broader outer-VM slowdown had also
been recorded before the PCI changes in the rebase results. Its cause is
not established; the GIC priority fix must not be claimed to resolve it.

These results establish initial discovery and working device I/O, not
general nested-virtualization reliability. All ramdisk runs used 512MB,
the outer VM stayed at 6GB/four CPUs, and Linux swap remained unused.

After restarting QEMU, the final cold-boot run
`/home/dv/bsd-rd-pci.OSfhh2tB` passed the same complete disk/network test on
the installed #185 kernel and matching vmd binary. This is a third successful
integration run, not a third consecutive pass. The test VM, daemon and TAP
were cleaned up; the outer development VM was left running for manual use.
