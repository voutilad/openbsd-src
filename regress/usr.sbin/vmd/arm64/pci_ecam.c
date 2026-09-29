/* $OpenBSD$ */
/* Public domain.  Exercise the actual userland PCI implementation. */
#include <sys/param.h>
#include <err.h>
#include <stdio.h>

#include "pci.c"

static struct vmd_vm vm;
struct vmd_vm *current_vm = &vm;
static unsigned int irq_high, irq_low, bar_calls;
static uint64_t bar_value;

#define CHECK(x) do { if (!(x)) errx(1, "line %d: %s", __LINE__, #x); } while (0)

void
vcpu_assert_irq(int fd, uint32_t cpu, int irq)
{
	CHECK(fd == 42 && cpu == 0 && irq == ARM64_PCI_INTID_BASE + 1);
	irq_high++;
}

void
vcpu_deassert_irq(int fd, uint32_t cpu, int irq)
{
	CHECK(fd == 42 && cpu == 0 && irq == ARM64_PCI_INTID_BASE + 1);
	irq_low++;
}

void
intr_toggle_el(struct vmd_vm *v, int irq, int level)
{
	(void)v;
	(void)irq;
	(void)level;
}

static int
bar(uint32_t cpu, int dir, uint32_t off, uint8_t size, uint64_t *data,
    void *cookie)
{
	CHECK(cpu == 0 && off == 0 && size == 4 && cookie == &vm);
	bar_calls++;
	if (dir == MMIO_DIR_WRITE)
		bar_value = *data;
	else
		*data = bar_value;
	return (0);
}

static uint64_t
cfg(int dir, unsigned int slot, unsigned int fn, unsigned int reg,
    uint8_t size, uint64_t data)
{
	CHECK(pci_handle_ecam(dir, ARM64_PCI_ECAM_BASE + (slot << 15) +
	    (fn << 12) + reg, size, &data) == 0);
	return (data);
}

int
main(void)
{
	uint8_t id;
	uint64_t data, base, moved;
	unsigned int i;

	vm.vm_fd = 42;
	pci_init();
	for (i = 1; i <= 5; i++) {
		CHECK(pci_add_device(&id, 0x1234, 0x5678, 0, 0, 0, 0,
		    1, 1, NULL) == 0 && id == i);
	}
	CHECK(cfg(MMIO_DIR_READ, 1, 0, 0, 4, 0) == 0x56781234);
	CHECK((cfg(MMIO_DIR_READ, 1, 0, 2, 2, 0) & 0xffff) == 0x5678);
	CHECK(cfg(MMIO_DIR_READ, 1, 1, 0, 4, 0) == UINT32_MAX);
	CHECK(cfg(MMIO_DIR_READ, 31, 0, 0, 4, 0) == UINT32_MAX);
	CHECK(cfg(MMIO_DIR_READ, 1, 0, 0x100, 4, 0) == 0);
	cfg(MMIO_DIR_WRITE, 1, 0, 0x100, 4, 0);
	cfg(MMIO_DIR_WRITE, 1, 0, 0, 4, 0);
	CHECK(cfg(MMIO_DIR_READ, 1, 0, 0, 4, 0) == 0x56781234);
	data = 0;
	CHECK(pci_handle_ecam(MMIO_DIR_READ, ARM64_PCI_ECAM_BASE + 1,
	    4, &data) == EINVAL);
	CHECK(pci_handle_ecam(MMIO_DIR_READ,
	    ARM64_PCI_ECAM_BASE + ARM64_PCI_ECAM_SIZE, 4, &data) == EINVAL);
	CHECK(pci_add_bar(1, PCI_MAPREG_TYPE_IO, NULL, NULL) == -1);
	CHECK(pci_add_bar(1, PCI_MAPREG_TYPE_MEM, bar, &vm) == 0);
	base = cfg(MMIO_DIR_READ, 1, 0, PCI_MAPREG_START, 4, 0);
	CHECK(base == ARM64_PCI_MEM_BASE);
	cfg(MMIO_DIR_WRITE, 1, 0, PCI_MAPREG_START, 4, UINT32_MAX);
	CHECK(cfg(MMIO_DIR_READ, 1, 0, PCI_MAPREG_START, 4, 0) ==
	    (uint32_t)~(VM_PCI_MMIO_BAR_SIZE - 1));
	moved = base + 2 * VM_PCI_MMIO_BAR_SIZE;
	cfg(MMIO_DIR_WRITE, 1, 0, PCI_MAPREG_START, 4, moved);
	CHECK(pci_handle_mmio(0, MMIO_DIR_READ, moved, 4, &data) == 0);
	CHECK(bar_calls == 0 && data == UINT64_MAX);
	cfg(MMIO_DIR_WRITE, 1, 0, PCI_COMMAND_STATUS_REG, 2,
	    PCI_COMMAND_MEM_ENABLE | PCI_COMMAND_MASTER_ENABLE);
	data = 0xdeadbeef;
	CHECK(pci_handle_mmio(0, MMIO_DIR_WRITE, moved, 4, &data) == 0);
	CHECK(bar_calls == 1 && bar_value == data);
	CHECK(pci_handle_mmio(0, MMIO_DIR_READ, base, 4, &data) == 0);
	CHECK(bar_calls == 1 && data == UINT64_MAX);

	/* Shared INTA lines remain asserted until every source is serviced. */
	pci_assert_irq(1, 0);
	pci_assert_irq(5, 0);
	CHECK(irq_high == 1 && irq_low == 0);
	pci_deassert_irq(1);
	CHECK(irq_low == 0);
	cfg(MMIO_DIR_WRITE, 5, 0, PCI_COMMAND_STATUS_REG, 2,
	    PCI_COMMAND_INTERRUPT_DISABLE);
	CHECK(irq_low == 1);
	cfg(MMIO_DIR_WRITE, 5, 0, PCI_COMMAND_STATUS_REG, 2, 0);
	CHECK(irq_high == 2);
	pci_deassert_irq(5);
	CHECK(irq_low == 2);
	puts("PCI_ECAM_INTX_PASS");
	return (0);
}
