#!/bin/sh
# Boot the arm64 ramdisk through vmd and drive install.sub to its disk probe.

set -eu

: "${VMD:=/usr/sbin/vmd}"

vmname="${VMNAME_PREFIX:-bsd-rd-hwfast}-$$"
console=/tmp/bsd-rd-hwfast.console
vmdlog=/tmp/bsd-rd-hwfast.vmd.log
vmdpid=
consolepid=
tty=

cleanup()
{
	set +e
	timeout 5 vmctl stop -f "${vmname}" >/dev/null 2>&1
	if [ -n "${consolepid}" ]; then
		kill "${consolepid}" >/dev/null 2>&1
		wait "${consolepid}" >/dev/null 2>&1
	fi
	if [ -n "${vmdpid}" ]; then
		kill "${vmdpid}" >/dev/null 2>&1
		wait "${vmdpid}" >/dev/null 2>&1
	fi
}

fail()
{
	echo "FAIL: $1" >&2
	tail -200 "${console}" >&2 2>/dev/null || true
	tail -100 "${vmdlog}" >&2 2>/dev/null || true
	exit 1
}

wait_for()
{
	pattern=$1
	limit=${2:-${WAIT_LIMIT:-300}}
	i=0
	while [ ${i} -lt "${limit}" ]; do
		grep -Fq "${pattern}" "${console}" && return 0
		kill -0 "${consolepid}" 2>/dev/null ||
		    fail "console reader exited waiting for: ${pattern}"
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

vmctl status 2>/dev/null | grep -q 'STATE NAME' &&
	fail 'another vmd instance is already running'

rm -f "${console}" "${vmdlog}"
${VMD} -d -f /dev/null >"${vmdlog}" 2>&1 &
vmdpid=$!

i=0
while [ ${i} -lt 100 ]; do
	vmctl status 2>/dev/null | grep -q 'STATE NAME' && break
	kill -0 "${vmdpid}" 2>/dev/null || fail 'vmd exited during startup'
	i=$((i + 1))
	sleep 0.1
done
vmctl status 2>/dev/null | grep -q 'STATE NAME' || fail 'vmd did not become ready'

startout="$(vmctl start -m 512M -b /home/dv/bsd.rd.vmm "${vmname}" 2>&1)" ||
	fail 'vmctl failed to start bsd.rd'
tty="$(printf '%s\n' "${startout}" | sed -n 's/^.*tty //p')"
[ -c "${tty}" ] || fail 'vmctl did not return a console device'

exec 3<>"${tty}"
stty raw -echo <&3
cat <&3 >"${console}" 2>&1 &
consolepid=$!

wait_for 'Welcome to the OpenBSD/arm64' "${BOOT_LIMIT:-300}"
wait_for '(I)nstall, (U)pgrade, (A)utoinstall or (S)hell?'
send i
wait_for 'Terminal type?'
send ''
wait_for "System hostname? (short form, e.g. 'foo')"
send vmm
wait_for "DNS domain name? (e.g. 'example.com')"
send ''
wait_for "DNS nameservers? (IP address list or 'none')"
send ''
wait_for 'Password for root account?'
send dv
wait_for 'Password for root account? (again)'
send dv
wait_for 'Start sshd(8) by default?'
send ''

# The console question is machine-dependent; answer it if it appears, while
# also accepting a build that advances directly to user setup.
i=0
while [ ${i} -lt 300 ]; do
	if grep -Fq 'Change the default console' "${console}"; then
		send ''
		break
	fi
	grep -Fq 'Setup a user?' "${console}" && break
	i=$((i + 1))
	sleep 0.2
done
wait_for 'Setup a user?'
send ''
wait_for 'Allow root ssh login?'
send ''
wait_for "What timezone are you in? ('?' for list)"
send ''
wait_for 'Available disks are: none.'
wait_for "Which disk is the root disk? ('?' for details)"

echo 'BSD_RD_BOOT_PASS'
echo '--- outer memory ---'
vmstat -s | egrep 'pages active|pages free|swap pages in use'
echo '--- vmd processes ---'
ps ax -o pid,stat,%cpu,rss,command | egrep '[v]md|[v]cpu' || true
echo '--- console tail ---'
tail -160 "${console}"
echo '--- vmd log ---'
cat "${vmdlog}"
