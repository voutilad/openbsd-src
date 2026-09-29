# Installer shell check, 2026-09-29

Outer kernel GENERIC.MP#182, 6GB; ramdisk guest 512MB. No source-tree or
kernel changes were made. The outer VM was initially stopped and was
started using puffy.sh for this test.

Selecting uppercase S returned a root shell prompt. The initial harness
used printf for completion markers, but printf is not present in this
stripped-down ramdisk. Its driver was stopped while a separate probe used
the echo builtin instead. Echo executed and printed PROBE_READY.

The next submitted line was `pwd; echo PWD_''DONE`. Only its initial `p`
was echoed; no further input/output appeared before the probe's 60-second
timeout. Thus shell startup and a builtin command are verified, but usable
interactive input is not reliable. This is not a successful full shell test.

After the timeout, the guest was paused to capture registers:

    pc=ffffff8000388b64       cpu_wfi
    pstate=614003c5
    elr_el1=ffffff8000379338  cpu_idle_cycle
    spsr_el1=61400305
    esr_el1=56000000
    ttbr0_el1=424c5000
    ttbr1_el1=416bf000

Symbols were resolved against the actual /home/dv/bsd.rd.vmm payload.
This snapshot shows the guest kernel idle; it is not evidence that pwd ran.

The original stopped driver was terminated and continued to execute its
cleanup, which stopped the nested VM and vmd. Its result file says
"console reader exited" because of that deliberate cleanup; the meaningful
probe failure was the timeout waiting for PWD_DONE. The console file is
the unedited transcript. Host swap stayed zero.

The accompanying bsd-rd-shell-test.sh now uses echo markers and sysctl
instead of relying on printf/uname in the ramdisk. Its corrected full
command sequence has not passed; the observed result remains the partial
shell success and input stall described above. The preserved console is
shell-console.log in this diagnostic directory.
