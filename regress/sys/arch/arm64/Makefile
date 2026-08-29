#	$OpenBSD$

.if ${MACHINE_ARCH} == "aarch64"
SUBDIR +=	vmm
.elif make(regress) || make(all)
${.TARGETS}:
	@echo Regress for architecture arm64 cannot run on ${MACHINE_ARCH}.
	@echo SKIPPED
.endif

install:

.include <bsd.subdir.mk>
