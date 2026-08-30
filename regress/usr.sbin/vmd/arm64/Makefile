#	$OpenBSD$

.if ${MACHINE_ARCH} != "aarch64"
regress:
	@echo SKIPPED
.else

VMD?=		/usr/sbin/vmd
VMCTL?=		/usr/sbin/vmctl

CLEANFILES+=	guest.o guest.elf console.out vmd.log

REGRESS_TARGETS=	run-regress-uart
REGRESS_ROOT_TARGETS=	${REGRESS_TARGETS}

guest.o: guest.S
	${CC} -c -o $@ ${.CURDIR}/guest.S

guest.elf: guest.o guest.ld
	${LD} -nopie -T ${.CURDIR}/guest.ld -o $@ guest.o

run-regress-uart: guest.elf
	${SUDO} env VMD=${VMD} VMCTL=${VMCTL} OBJDIR=${.OBJDIR} \
	    sh ${.CURDIR}/uart.sh

.endif

.include <bsd.regress.mk>
