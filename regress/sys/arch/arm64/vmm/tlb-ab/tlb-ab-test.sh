#!/bin/sh
# Temporary diagnostic driver for the self-timed vmm entry-TLBI experiment.
# Run only one 512MB ramdisk VM, with the matching diagnostic kernel/helper.
set -eu

vmid=${1:?kernel VM id required}
order=${2:?forward, reverse, icache or icache-reverse required}
case ${order} in forward|reverse|icache|icache-reverse) ;; *) exit 2 ;; esac
out=$(mktemp -d /home/dv/vmm-tlb-${order}.XXXXXXXX)
chown dv "${out}"
bootpid=
cleanup()
{
	if [ -n "${bootpid}" ]; then
		kill -TERM "${bootpid}" 2>/dev/null || true
		wait "${bootpid}" 2>/dev/null || true
	fi
}
trap cleanup EXIT HUP INT TERM
printf 'RESULT_DIRECTORY=%s\n' "${out}"
uname -a >"${out}/kernel"
env BOOT_LIMIT=2400 WAIT_LIMIT=300 VMNAME_PREFIX="bsd-rd-tlb-${order}" \
    sh /tmp/bsd-rd-test.sh >"${out}/boot-result" 2>&1 &
bootpid=$!

# Sample without READREGS, pausing, or changing the guest's RAM contents.
# SHAREMEM only reads already allocated guest kernel counter pages.
i=0
finished=0
while [ ${i} -lt 90 ] && kill -0 "${bootpid}" 2>/dev/null; do
	{
		date -u '+SAMPLE %Y-%m-%dT%H:%M:%SZ'
		/tmp/vmm-inspect "${vmid}" counters 2>>"${out}/reader-errors" || true
		dmesg | awk -v id="${vmid}" '
		    /^vmm diag: vm/ { wanted = ($4 == id) }
		    /^vmm diag:/ && wanted { print }
		' | tail -4
	} >>"${out}/samples"
	if tail -1 "${out}/samples" | grep -q 'phase 5 mode 0 el0_started 1'; then
		finished=$((finished + 1))
		[ ${finished} -ge 4 ] && break
	fi
	i=$((i + 1))
	sleep 5
done

# The timed experiment is over.  Only now pause to inspect the final state.
if kill -0 "${bootpid}" 2>/dev/null; then
	timeout 10 vmctl pause 1 >"${out}/pause" 2>&1 || true
	timeout 10 /tmp/vmm-inspect "${vmid}" walk \
	    >"${out}/registers-and-walk" 2>&1 || true
fi
dmesg >"${out}/dmesg"
if [ -f /tmp/bsd-rd-hwfast.console ]; then
	cp /tmp/bsd-rd-hwfast.console "${out}/console"
fi
cp /tmp/bsd-rd-hwfast.vmd.log "${out}/vmd-log"
cleanup
bootpid=
printf 'EXPERIMENT_FINISHED final_phase_samples=%s directory=%s\n' \
    "${finished}" "${out}"
[ ${finished} -ge 4 ]
