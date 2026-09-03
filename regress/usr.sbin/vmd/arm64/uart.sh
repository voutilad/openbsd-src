#!/bin/sh
#	$OpenBSD$

set -eu

: "${VMD:=/usr/sbin/vmd}"
: "${VMCTL:=/usr/sbin/vmctl}"
: "${OBJDIR:=$(pwd)}"

vmname="regress-vmd-arm64-$$"
console="${OBJDIR}/console.out"
vmdlog="${OBJDIR}/vmd.log"
vmdpid=
consolepid=
startout=
tty=

cleanup()
{
	set +e
	# No vmmci exists on arm64 yet, so request a bounded forced stop.
	timeout 3 ${VMCTL} stop -f "${vmname}" >/dev/null 2>&1
	if [ -n "${vmdpid}" ]; then
		kill "${vmdpid}" >/dev/null 2>&1
		wait "${vmdpid}" >/dev/null 2>&1
	fi
	if [ -n "${consolepid}" ]; then
		kill "${consolepid}" >/dev/null 2>&1
		wait "${consolepid}" >/dev/null 2>&1
	fi
}

fail()
{
	echo "$1" >&2
	[ ! -s "${console}" ] || cat "${console}" >&2
	[ ! -s "${vmdlog}" ] || cat "${vmdlog}" >&2
	exit 1
}

vmd_ready()
{
	# vmctl reports status 1 for a valid, empty VM list.
	${VMCTL} status 2>/dev/null | grep -q "STATE NAME"
}

trap cleanup EXIT HUP INT TERM

if vmd_ready; then
	# The control socket has a fixed path; never disrupt a running service.
	fail "a vmd instance is already using /var/run/vmd.sock"
fi

rm -f "${console}" "${vmdlog}"
# Foreground/debug mode gives this script one parent PID to reap reliably.
${VMD} -d -f /dev/null >"${vmdlog}" 2>&1 &
vmdpid=$!

i=0
while [ ${i} -lt 50 ]; do
	if vmd_ready; then
		break
	fi
	if ! kill -0 "${vmdpid}" 2>/dev/null; then
		fail "vmd exited during startup"
	fi
	i=$((i + 1))
	sleep 0.1
done
vmd_ready || fail "vmd did not become ready"

startout="$(${VMCTL} start -m 64M -b "${OBJDIR}/guest.elf" \
    "${vmname}" 2>&1)" ||
	fail "vmctl failed to start the payload"
tty="$(printf '%s\n' "${startout}" | sed -n 's/^.*tty //p')"
[ -c "${tty}" ] || fail "vmctl did not return a console device"

# Keep one read/write descriptor so this script can both capture guest output
# and inject the byte used to exercise the PL011 receive interrupt.
exec 3<>"${tty}"
stty raw -echo <&3
cat <&3 >"${console}" 2>&1 &
consolepid=$!

i=0
while [ ${i} -lt 100 ]; do
	grep -q "arm64 vmd waiting for PL011 input" "${console}" && break
	kill -0 "${consolepid}" 2>/dev/null || fail "console reader exited"
	i=$((i + 1))
	sleep 0.1
done
grep -q "arm64 vmd waiting for PL011 input" "${console}" ||
	fail "guest did not enable PL011 receive interrupts"
printf x >&3

i=0
while [ ${i} -lt 50 ]; do
	grep -q "arm64 vmd PL011 RX interrupt works" "${console}" && break
	i=$((i + 1))
	sleep 0.1
done
kill "${consolepid}" >/dev/null 2>&1 || true
wait "${consolepid}" >/dev/null 2>&1 || true
consolepid=
exec 3>&-

grep -q "arm64 vmd FDT + GICv3 SPI interrupt works" "${console}" ||
	fail "guest did not take, acknowledge, and EOI the FDT-described GICv3 SPI"
grep -q "arm64 vmd hardware virtual timer PPI works" "${console}" ||
	fail "guest did not take and clear the FDT-described virtual timer PPI"
grep -q "arm64 vmd repeated timer wakeups work" "${console}" ||
	fail "guest lost a repeated virtual timer wakeup"
grep -q "arm64 vmd timer preempted an active SPI" "${console}" ||
	fail "guest timer did not preempt an interrupt active in another LR"
grep -q "arm64 vmd PL011 TX interrupt works" "${console}" ||
	fail "guest did not take the PL011 transmit-ready interrupt"
grep -q "arm64 vmd PL011 RX interrupt works" "${console}" ||
	fail "guest did not receive the PTY byte through a PL011 interrupt"
${VMCTL} status "${vmname}" | grep -q "${vmname}" ||
	fail "guest did not remain halted after HVC"
