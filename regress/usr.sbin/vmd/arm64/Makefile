#	$OpenBSD$

.if ${MACHINE_ARCH} != "aarch64"
regress:
	@echo SKIPPED
.else

VMD?=		/usr/sbin/vmd
VMCTL?=		/usr/sbin/vmctl

CLEANFILES+=	guest.o guest_start.o guest.elf console.out vmd.log

REGRESS_TARGETS=	run-regress-fdt-gic-uart
REGRESS_ROOT_TARGETS=	${REGRESS_TARGETS}

guest_start.o: guest.S
	${CC} -c -o $@ ${.CURDIR}/guest.S

guest.o: guest.c
	${CC} -O2 -Wall -Wextra -ffreestanding -fno-builtin \
	    -fno-stack-protector \
	    -fno-pie -fno-unwind-tables -fno-asynchronous-unwind-tables \
	    -c -o $@ ${.CURDIR}/guest.c

guest.elf: guest_start.o guest.o guest.ld
	${LD} -nopie -T ${.CURDIR}/guest.ld -o $@ guest_start.o guest.o

run-regress-fdt-gic-uart: guest.elf
	${SUDO} env VMD=${VMD} VMCTL=${VMCTL} OBJDIR=${.OBJDIR} \
	    sh ${.CURDIR}/uart.sh

.endif

.include <bsd.regress.mk>
