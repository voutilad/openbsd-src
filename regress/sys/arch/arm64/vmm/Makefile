#	$OpenBSD$

.if "${MACHINE_ARCH}" != aarch64
regress:
	@echo SKIPPED
.else

PROGS =			vcpu preempt
WARNINGS =		yes

REGRESS_TARGETS =	run-regress-vcpu run-regress-preempt
run-regress-vcpu: vcpu
	${SUDO} ./vcpu

run-regress-preempt: preempt
	${SUDO} ./preempt

.if ! (make(clean) || make(cleandir) || make(obj))
.include <bsd.own.mk>
VMMDEV_FAIL !=		/bin/test -c /dev/vmm; echo $$?

.if "${VMMDEV_FAIL}" != 0
REGRESS_SKIP_TARGETS =	${REGRESS_TARGETS}
.endif
.endif
.endif

.include <bsd.regress.mk>
