#	$OpenBSD$

.if "${MACHINE_ARCH}" != aarch64
regress:
	@echo SKIPPED
.else

PROGS =			vcpu preempt inject sysreg timer fp pauth tlbi
SRCS_fp =		fp.c fp_asm.S
SRCS_tlbi =		tlbi.c tlbi_asm.S
WARNINGS =		yes

REGRESS_TARGETS =	run-regress-vcpu run-regress-preempt run-regress-inject \
			run-regress-sysreg run-regress-timer run-regress-fp \
			run-regress-pauth run-regress-tlbi
run-regress-vcpu: vcpu
	${SUDO} ./vcpu

run-regress-preempt: preempt
	${SUDO} ./preempt

run-regress-inject: inject
	${SUDO} ./inject

run-regress-sysreg: sysreg
	${SUDO} ./sysreg

run-regress-timer: timer
	${SUDO} ./timer

run-regress-fp: fp
	${SUDO} ./fp

run-regress-pauth: pauth
	${SUDO} ./pauth

run-regress-tlbi: tlbi
	${SUDO} ./tlbi

.if ! (make(clean) || make(cleandir) || make(obj))
.include <bsd.own.mk>
VMMDEV_FAIL !=		/bin/test -c /dev/vmm; echo $$?

.if "${VMMDEV_FAIL}" != 0
REGRESS_SKIP_TARGETS =	${REGRESS_TARGETS}
.endif
.endif
.endif

.include <bsd.regress.mk>
