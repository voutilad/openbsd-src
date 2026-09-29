#	$OpenBSD$

.if ${MACHINE_ARCH} != "aarch64"
regress:
	@echo SKIPPED
.else

VMD?=		/usr/sbin/vmd
VMCTL?=		/usr/sbin/vmctl

CLEANFILES+=	guest.o guest_start.o guest.elf console.out vmd.log timer_schedule
CLEANFILES+=	pci_ecam

REGRESS_TARGETS=	run-regress-timer-schedule run-regress-fdt-gic-uart
REGRESS_TARGETS+= run-regress-pci-ecam
REGRESS_ROOT_TARGETS=	run-regress-fdt-gic-uart

VMD_SRC=	${.CURDIR}/../../../../usr.sbin/vmd
pci_ecam: pci_ecam.c ${VMD_SRC}/pci.c ${VMD_SRC}/pci.h
	${CC} -Wall -Wextra -I${VMD_SRC} -o $@ \
	    ${.CURDIR}/pci_ecam.c ${VMD_SRC}/log.c -lpthread

run-regress-pci-ecam: pci_ecam
	./pci_ecam

timer_schedule: timer_schedule.c ${VMD_SRC}/arm64_timer.c ${VMD_SRC}/log.c
	${CC} -Wall -Wextra -I${VMD_SRC} -o $@ \
	    ${.CURDIR}/timer_schedule.c ${VMD_SRC}/log.c -lpthread -levent

run-regress-timer-schedule: timer_schedule
	./timer_schedule

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
