#!/bin/sh
# Bounded positive-control boot, with live counters and no mid-run pause.
set -eu
vmid=${1:?kernel VM id required}
out=$(mktemp -d /home/dv/vmm-tlb-positive.XXXXXXXX)
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
env BOOT_LIMIT=3000 WAIT_LIMIT=300 VMNAME_PREFIX=bsd-rd-tlb-continuous \
    sh /tmp/bsd-rd-test.sh >"${out}/boot-result" 2>&1 &
bootpid=$!
i=0
while kill -0 "${bootpid}" 2>/dev/null && [ ${i} -lt 180 ]; do
	{
		date -u '+SAMPLE %Y-%m-%dT%H:%M:%SZ'
		/tmp/vmm-inspect "${vmid}" counters 2>>"${out}/reader-errors" || true
		dmesg | awk -v id="${vmid}" '
		    /^vmm diag: vm/ { wanted = ($4 == id) }
		    /^vmm diag:/ && wanted { print }
		' | tail -4
	} >>"${out}/samples"
	i=$((i + 1))
	sleep 5
done
if kill -0 "${bootpid}" 2>/dev/null; then
	printf 'FAIL: positive-control wall-time limit\n' >>"${out}/boot-result"
	cleanup
	ret=1
else
	ret=0
	wait "${bootpid}" || ret=$?
fi
bootpid=
dmesg >"${out}/dmesg"
if [ -f /tmp/bsd-rd-hwfast.console ]; then
	cp /tmp/bsd-rd-hwfast.console "${out}/console"
fi
cp /tmp/bsd-rd-hwfast.vmd.log "${out}/vmd-log"
printf 'POSITIVE_CONTROL_FINISHED status=%s directory=%s\n' "${ret}" "${out}"
exit "${ret}"
