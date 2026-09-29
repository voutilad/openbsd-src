/*
 * Temporary diagnostic utility, not installed or part of the vmm ABI.
 * The launcher must pass an inherited VM fd; opening /dev/vmm no longer
 * grants access to another process's VM by its former numeric ID.
 * Memory inspection still assumes the original 512MB diagnostic layout
 * and, for counters, the exact historic bsd.rd payload's symbol offsets.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <machine/vmmvar.h>
#include <dev/vmm/vmm.h>
#include <uvm/uvmexp.h>
#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
walk(int fd, const struct vcpu_reg_state *r)
{
	struct vm_sharemem_params s;
	uint64_t table, pte, pa;
	unsigned int level, shift;
	uint32_t insn;
	char *ram;

	memset(&s, 0, sizeof(s));
	if (ioctl(fd, VMM_IOC_SHAREMEM, &s) == -1)
		err(1, "SHAREMEM (requires the 512MB vmd diagnostic layout)");
	ram = (char *)s.vsp_va[3];
	if (r == NULL) {
		/* Symbol offsets of this specific /home/dv/bsd.rd.vmm payload. */
		struct uvmexp u;
		uint64_t uptime;
		unsigned int ticks;
		memcpy(&u, ram + 0x1668048, sizeof(u));
		memcpy(&ticks, ram + 0x1600ab8, sizeof(ticks));
		memcpy(&uptime, ram + 0x162b7c0, sizeof(uptime));
		printf("traps=%d intrs=%d syscalls=%d switches=%d "
		    "ticks=%u uptime=%llu\n", u.traps, u.intrs, u.syscalls,
		    u.swtch, ticks, (unsigned long long)uptime);
		return;
	}
	table = r->vrs_ttbr0_el1 & 0xfffffffff000ULL;
	for (level = 1; level <= 3; level++) {
		shift = 12 + (3 - level) * 9;
		pa = table + ((r->vrs_pc >> shift) & 511) * 8;
		if (pa < 0x40000000 || pa >= 0x60000000)
			errx(1, "page table outside RAM: %llx",
			    (unsigned long long)pa);
		memcpy(&pte, ram + pa - 0x40000000, sizeof(pte));
		printf("L%u PTE at %llx = %llx\n", level,
		    (unsigned long long)pa, (unsigned long long)pte);
		if ((pte & 3) != 3)
			break;
		table = pte & 0xfffffffff000ULL;
	}
	if (level == 4) {
		pa = table + (r->vrs_pc & 0xfff);
		if (pa >= 0x40000000 && pa <= 0x60000000 - sizeof(insn)) {
			memcpy(&insn, ram + pa - 0x40000000, sizeof(insn));
			printf("instruction in RAM at IPA %llx = %08x\n",
			    (unsigned long long)pa, insn);
		}
	}
}

int
main(int argc, char **argv)
{
	struct vm_rwregs_params p;
	struct vm_irqcfg_params q;
	struct vcpu_reg_state *r = &p.vrwp_regs;
	const char *error;
	unsigned int i;
	int fd;

	if (argc < 2 || argc > 3)
		errx(1, "usage: vmm-inspect inherited-fd "
		    "[walk|counters|timer-off|timer-on]");
	fd = strtonum(argv[1], 0, INT_MAX, &error);
	if (error)
		errx(1, "fd: %s", error);
	if (fcntl(fd, F_GETFD) == -1)
		err(1, "inherited VM descriptor");
	if (argc == 3 && strcmp(argv[2], "counters") == 0) {
		walk(fd, NULL);
		return (0);
	}
	if (argc == 3 && strcmp(argv[2], "walk") != 0) {
		memset(&q, 0, sizeof(q));
		q.viq_intr = VMM_ARM64_TIMER_INTID;
		q.viq_priority = 0x50;
		if (strcmp(argv[2], "timer-on") == 0)
			q.viq_flags = VMM_IRQCFG_ENABLED;
		else if (strcmp(argv[2], "timer-off") != 0)
			errx(1, "unknown operation");
		if (ioctl(fd, VMM_IOC_IRQCFG, &q) == -1)
			err(1, "IRQCFG");
		return (0);
	}
	memset(&p, 0, sizeof(p));
	p.vrwp_mask = VM_RWREGS_ALL;
	if (ioctl(fd, VMM_IOC_READREGS, &p) == -1)
		err(1, "READREGS");
#define PRINT(f) printf(#f "=%llx\n", (unsigned long long)r->vrs_##f)
	PRINT(pc); PRINT(pstate); PRINT(sp); PRINT(sp_el0);
	PRINT(elr_el1); PRINT(spsr_el1); PRINT(esr_el1); PRINT(far_el1);
	PRINT(sctlr_el1); PRINT(tcr_el1); PRINT(ttbr0_el1); PRINT(ttbr1_el1);
	for (i = 0; i < 31; i++)
		printf("x%u=%llx%c", i, (unsigned long long)r->vrs_gprs[i],
		    i % 3 == 2 ? '\n' : ' ');
	putchar('\n');
	if (argc == 3)
		walk(fd, r);
	return (0);
}
