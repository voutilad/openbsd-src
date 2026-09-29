#!/bin/sh
# Boot an unchanged ramdisk with disposable PCI block and network devices.
# Requires root and exclusive use of vmd and the tap interface group.
set -eu

out=$(mktemp -d /home/dv/bsd-rd-pci.XXXXXXXX)
vmname="bsd-rd-pci-$$"
console=${out}/console
vmdpid=
consolepid=
created=0
bsd_rd=${BSD_RD:-/bsd.rd}

cleanup()
{
	set +e
	if [ "${created}" = 1 ]; then
		timeout 10 vmctl stop -f "${vmname}" >/dev/null 2>&1
	fi
	if [ -n "${consolepid}" ]; then
		kill "${consolepid}" >/dev/null 2>&1
		wait "${consolepid}" >/dev/null 2>&1
	fi
	if [ -n "${vmdpid}" ]; then
		kill "${vmdpid}" >/dev/null 2>&1
		wait "${vmdpid}" >/dev/null 2>&1
	fi
	chown -R dv "${out}"
}

fail()
{
	echo "FAIL: $1" | tee -a "${out}/result"
	tail -70 "${console}" 2>/dev/null || true
	exit 1
}

wait_for()
{
	pattern=$1
	limit=${2:-1500}
	i=0
	while [ "${i}" -lt "${limit}" ]; do
		grep -Fq "${pattern}" "${console}" && return 0
		kill -0 "${consolepid}" 2>/dev/null || fail 'console reader exited'
		i=$((i + 1))
		sleep 0.2
	done
	fail "timed out waiting for: ${pattern}"
}

send()
{
	printf '%s\r' "$1" >&3
}

trap cleanup EXIT HUP INT TERM
echo "RESULT_DIRECTORY=${out}"
uname -a >"${out}/kernel"
sha256 "${bsd_rd}" >"${out}/payload"
date -u >"${out}/started"
if vmctl status 2>/dev/null | grep -q 'STATE NAME'; then
	fail 'another vmd instance is running'
fi
if [ -n "$(ifconfig -a | sed -n 's/^\(tap[0-9][0-9]*\):.*/\1/p')" ]; then
	fail 'existing TAP interfaces; refusing to change their configuration'
fi

# All writable storage belongs to this run; never attach a user's disk.
vmctl create -s 64M "${out}/disk.raw"
dd if=/dev/urandom of="${out}/sector" bs=512 count=1 2>/dev/null
dd if="${out}/sector" of="${out}/disk.raw" conv=notrunc 2>/dev/null
digest=$(sha256 -q "${out}/sector")

/usr/sbin/vmd -d -v -f /dev/null >"${out}/vmd.log" 2>&1 &
vmdpid=$!
i=0
while [ "${i}" -lt 100 ]; do
	vmctl status 2>/dev/null | grep -q 'STATE NAME' && break
	kill -0 "${vmdpid}" 2>/dev/null || fail 'vmd exited during startup'
	i=$((i + 1))
	sleep 0.1
done
vmctl status 2>/dev/null | grep -q 'STATE NAME' || fail 'vmd not ready'
startout=$(vmctl start -m 512M -b "${bsd_rd}" -d "${out}/disk.raw" \
    -i 1 "${vmname}" 2>&1) || fail "vmctl start failed: ${startout}"
created=1
tty=$(printf '%s\n' "${startout}" | sed -n 's/^.*tty //p')
[ -c "${tty}" ] || fail 'missing console device'
exec 3<>"${tty}"
stty raw -echo <&3
cat <&3 >"${console}" 2>&1 &
consolepid=$!
i=0
while [ "${i}" -lt 100 ]; do
	tap=$(ifconfig -a | sed -n 's/^\(tap[0-9][0-9]*\):.*/\1/p')
	[ "$(printf '%s\n' "${tap}" | wc -w)" -eq 1 ] && break
	i=$((i + 1))
	sleep 0.1
done
ifconfig -a >"${out}/interfaces"
[ "$(printf '%s\n' "${tap}" | wc -w)" -eq 1 ] || fail 'expected one TAP'
ifconfig "${tap}" inet 198.18.0.1/30 up
ifconfig "${tap}" >"${out}/tap"

wait_for 'pci0 at pciecam0'
wait_for 'vioblk0 at virtio'
wait_for 'vio0 at virtio'
wait_for '(I)nstall, (U)pgrade, (A)utoinstall or (S)hell?'
send S
wait_for '# '
send "sysctl hw.disknames && echo DISKS_''DONE"
wait_for 'DISKS_DONE'
send "(cd /dev && sh MAKEDEV sd0) && echo DEVNODES_''DONE"
wait_for 'DEVNODES_DONE'
send "dd if=/dev/rsd0c of=/tmp/sector bs=512 count=1 && echo READ_''DONE"
wait_for 'READ_DONE'
send 'sha256 -q /tmp/sector'
wait_for "${digest}"

# Round-trip the random sector through a different location on the new disk.
send "dd if=/tmp/sector of=/dev/rsd0c bs=512 seek=8 count=1 && echo WRITE_''DONE"
wait_for 'WRITE_DONE'
send "dd if=/dev/rsd0c of=/tmp/roundtrip bs=512 skip=8 count=1 && echo VERIFY_''DONE"
wait_for 'VERIFY_DONE'
send 'echo ROUNDTRIP_"$(sha256 -q /tmp/roundtrip)"'
wait_for "ROUNDTRIP_${digest}"
send "ifconfig vio0 inet 198.18.0.2/30 up && echo IFCONFIG_''DONE"
wait_for 'IFCONFIG_DONE'
send "ping -n -c 5 198.18.0.1 && echo NET_''PASS"
wait_for 'NET_PASS'
send "sleep 1 && echo TIMER_''PASS"
wait_for 'TIMER_PASS'
echo BSD_RD_PCI_PASS | tee "${out}/result"
date -u >"${out}/finished"
tail -90 "${console}"
