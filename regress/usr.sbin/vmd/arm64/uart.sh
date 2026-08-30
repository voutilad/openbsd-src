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
}

fail()
{
	echo "$1" >&2
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

# guest.S has a bounded delay so this reader is attached before PL011 output.
timeout 7 cat "${tty}" >"${console}" 2>&1 || true

grep -q "arm64 vmd PL011 works" "${console}" ||
	fail "guest did not write the expected PL011 message"
${VMCTL} status "${vmname}" | grep -q "${vmname}" ||
	fail "guest did not remain halted after HVC"
