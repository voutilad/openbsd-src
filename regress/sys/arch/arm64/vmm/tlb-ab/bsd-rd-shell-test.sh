#!/bin/sh
# Non-destructive check of the installer's S option.
set -eu

out=$(mktemp -d /home/dv/bsd-rd-shell.XXXXXXXX)
vmname="bsd-rd-shell-$$"
console=${out}/console
vmdpid=
consolepid=
created=0
bsd_rd=${BSD_RD:-/home/dv/bsd.rd.vmm}

cleanup()
{
	set +e
	if [ "${created}" = 1 ]; then
		timeout 5 vmctl stop -f "${vmname}" >/dev/null 2>&1
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
	limit=${2:-900}
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

/usr/sbin/vmd -d -f /dev/null >"${out}/vmd.log" 2>&1 &
vmdpid=$!
i=0
while [ "${i}" -lt 100 ]; do
	vmctl status 2>/dev/null | grep -q 'STATE NAME' && break
	kill -0 "${vmdpid}" 2>/dev/null || fail 'vmd exited during startup'
	i=$((i + 1))
	sleep 0.1
done
vmctl status 2>/dev/null | grep -q 'STATE NAME' || fail 'vmd not ready'
startout=$(vmctl start -m 512M -b "${bsd_rd}" "${vmname}" 2>&1) ||
	fail 'vmctl start failed'
created=1
tty=$(printf '%s\n' "${startout}" | sed -n 's/^.*tty //p')
[ -c "${tty}" ] || fail 'missing console device'
exec 3<>"${tty}"
stty raw -echo <&3
cat <&3 >"${console}" 2>&1 &
consolepid=$!

wait_for 'Welcome to the OpenBSD/arm64' 1500
wait_for '(I)nstall, (U)pgrade, (A)utoinstall or (S)hell?'
send S
wait_for '# '

# Quote concatenation keeps markers out of echoed input. The ramdisk's
# stripped-down shell does not provide printf or every base-system utility.
send "echo SHELL_''READY"
wait_for 'SHELL_READY'
send "pwd && echo PWD_''DONE"
wait_for 'PWD_DONE'
send "ls / && echo LS_''DONE"
wait_for 'LS_DONE'
send "sysctl kern.ostype kern.osrelease && echo SYSCTL_''DONE"
wait_for 'SYSCTL_DONE'
send 'echo ARITH_"$((6 * 7))"'
wait_for 'ARITH_42'
send "sleep 1 && echo SLEEP_''DONE"
wait_for 'SLEEP_DONE'
echo BSD_RD_SHELL_PASS | tee "${out}/result"
date -u >"${out}/finished"
tail -70 "${console}"
