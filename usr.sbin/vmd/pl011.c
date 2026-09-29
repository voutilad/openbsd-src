/*	$OpenBSD$	*/
/*
 * Copyright (c) 2026 Dave Voutila <dv@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/ttycom.h>

#include <errno.h>
#include <event.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "vmd.h"
#include "arm64_vm.h"
#include "gicv3.h"
#include "pl011.h"

#define PL011_DR		0x00
#define  PL011_DR_BE		(1U << 10)
#define PL011_RSR_ECR		0x04
#define PL011_FR		0x18
#define  PL011_FR_RXFE		(1U << 4)
#define  PL011_FR_TXFE		(1U << 7)
#define PL011_IMSC		0x38
#define PL011_RIS		0x3c
#define PL011_MIS		0x40
#define PL011_ICR		0x44
#define PL011_DMACR		0x48
#define PL011_REG_LAST		PL011_DMACR

#define PL011_INT_RX		(1U << 4)
#define PL011_INT_TX		(1U << 5)
#define PL011_INT_MASK		0x7ff

struct pl011_dev {
	pthread_mutex_t	 pd_mtx;
	int		 pd_fd;
	int		 pd_vm_fd;
	uint32_t	 pd_regs[(PL011_REG_LAST / sizeof(uint32_t)) + 1];
	uint16_t	 pd_rx_data;
	uint32_t	 pd_ris;
	int		 pd_rx_full;
	int		 pd_irq_line;
	int		 pd_connected;
	int		 pd_paused;
	struct event	 pd_event;
	struct event	 pd_wake;
	struct vm_dev_pipe pd_pipe;
};

static struct pl011_dev pl011;

static int	pl011_drive_locked(void);
static void	pl011_pipe_dispatch(int, short, void *);
static void	pl011_rcv_event(int, short, void *);
static void	pl011_rearm_locked(void);

static int
pl011_drive_locked(void)
{
	uint32_t imsc, mis;
	int irq_line;

	imsc = pl011.pd_regs[PL011_IMSC / sizeof(uint32_t)];
	mis = (pl011.pd_ris | PL011_INT_TX) & imsc;
	irq_line = mis != 0;
	if (irq_line == pl011.pd_irq_line)
		return (0);
	if (gicv3_set_irq(pl011.pd_vm_fd, 0, ARM64_UART_INTID,
	    irq_line) != 0)
		return (EIO);
	pl011.pd_irq_line = irq_line;
	return (0);
}

static void
pl011_rearm_locked(void)
{
	event_del(&pl011.pd_event);
	event_del(&pl011.pd_wake);
	if (pl011.pd_paused)
		return;
	if (!pl011.pd_connected)
		event_add(&pl011.pd_wake, NULL);
	else if (!pl011.pd_rx_full)
		event_add(&pl011.pd_event, NULL);
}

static void
pl011_pipe_dispatch(int fd, short event, void *arg)
{
	enum pipe_msg_type msg;

	(void)fd;
	(void)event;
	(void)arg;

	msg = vm_pipe_recv(&pl011.pd_pipe);
	if (msg != PL011_RX_RESUME)
		fatalx("%s: unexpected pipe message %d", __func__, msg);
	mutex_lock(&pl011.pd_mtx);
	pl011_rearm_locked();
	mutex_unlock(&pl011.pd_mtx);
}

static void
pl011_rcv_event(int fd, short kind, void *arg)
{
	uint8_t buf[2];
	uint16_t data = 0;
	ssize_t n;
	int have_data = 0;

	(void)arg;

	mutex_lock(&pl011.pd_mtx);
	if ((kind & EV_WRITE) != 0) {
		pl011.pd_connected = 1;
		if (!pl011.pd_paused && !pl011.pd_rx_full)
			event_add(&pl011.pd_event, NULL);
		mutex_unlock(&pl011.pd_mtx);
		return;
	}
	n = read(fd, buf, sizeof(buf));
	if (n == -1) {
		if (errno != EAGAIN && errno != EINTR)
			log_warn("unexpected read error on PL011 console");
	} else if (n == 0) {
		pl011.pd_connected = 0;
		event_del(&pl011.pd_event);
		if (!pl011.pd_paused)
			event_add(&pl011.pd_wake, NULL);
	} else if (n == 2) {
		switch (buf[0]) {
		case 0:
			data = buf[1];
			have_data = 1;
			break;
		case TIOCUCNTL_SBRK:
			data = PL011_DR_BE;
			have_data = 1;
			break;
		case TIOCUCNTL_CBRK:
			break;
		default:
			log_warnx("unexpected PL011 UCNTL command: %u", buf[0]);
			break;
		}
	} else if (n != 1)
		log_warnx("unexpected PL011 console read size %zd", n);

	if (have_data) {
		event_del(&pl011.pd_event);
		pl011.pd_rx_data = data;
		pl011.pd_rx_full = 1;
		pl011.pd_ris |= PL011_INT_RX;
		if (pl011_drive_locked() != 0)
			log_warnx("failed to assert PL011 interrupt");
	}
	mutex_unlock(&pl011.pd_mtx);
}

void
pl011_init(int fd, int vm_fd)
{
	int error;

	memset(&pl011, 0, sizeof(pl011));
	error = pthread_mutex_init(&pl011.pd_mtx, NULL);
	if (error != 0) {
		errno = error;
		fatal("could not initialize PL011 mutex");
	}
	pl011.pd_fd = fd;
	pl011.pd_vm_fd = vm_fd;
	event_set(&pl011.pd_event, fd, EV_READ | EV_PERSIST,
	    pl011_rcv_event, NULL);
	event_set(&pl011.pd_wake, fd, EV_WRITE, pl011_rcv_event, NULL);
	vm_pipe_init(&pl011.pd_pipe, pl011_pipe_dispatch);
	event_add(&pl011.pd_pipe.read_ev, NULL);
	event_add(&pl011.pd_wake, NULL);
}

void
pl011_pause(void)
{
	mutex_lock(&pl011.pd_mtx);
	pl011.pd_paused = 1;
	pl011_rearm_locked();
	mutex_unlock(&pl011.pd_mtx);
}

void
pl011_unpause(void)
{
	mutex_lock(&pl011.pd_mtx);
	pl011.pd_paused = 0;
	pl011_rearm_locked();
	mutex_unlock(&pl011.pd_mtx);
}

int
pl011_mmio(paddr_t gpa, size_t len, int iswrite, uint32_t *data)
{
	paddr_t offset;
	uint8_t ch;
	uint32_t raw;
	ssize_t n;
	int error = 0, resume = 0;

	if (data == NULL || gpa < ARM64_UART_BASE ||
	    gpa - ARM64_UART_BASE >= ARM64_UART_SIZE)
		return (EFAULT);
	offset = gpa - ARM64_UART_BASE;
	if ((len != sizeof(uint16_t) && len != sizeof(uint32_t)) ||
	    (offset & (len - 1)) != 0 ||
	    (len != sizeof(uint32_t) && offset != PL011_DR))
		return (EINVAL);

	mutex_lock(&pl011.pd_mtx);
	switch (offset) {
	case PL011_DR:
		if (iswrite) {
			ch = *data;
			do {
				n = write(pl011.pd_fd, &ch, sizeof(ch));
			} while (n == -1 && errno == EINTR);
		} else {
			*data = pl011.pd_rx_full ? pl011.pd_rx_data : 0;
			if (pl011.pd_rx_full) {
				pl011.pd_rx_full = 0;
				pl011.pd_ris &= ~PL011_INT_RX;
				resume = 1;
			}
			error = pl011_drive_locked();
		}
		break;
	case PL011_RSR_ECR:
		if (iswrite)
			pl011.pd_rx_data &= 0xff;
		else
			*data = (pl011.pd_rx_data >> 8) & 0xf;
		break;
	case PL011_FR:
		if (!iswrite) {
			*data = PL011_FR_TXFE;
			if (!pl011.pd_rx_full)
				*data |= PL011_FR_RXFE;
		}
		break;
	case PL011_IMSC:
		if (iswrite) {
			pl011.pd_regs[offset / sizeof(uint32_t)] =
			    *data & PL011_INT_MASK;
			error = pl011_drive_locked();
		} else
			*data = pl011.pd_regs[offset / sizeof(uint32_t)];
		break;
	case PL011_RIS:
		if (!iswrite)
			*data = pl011.pd_ris | PL011_INT_TX;
		break;
	case PL011_MIS:
		if (!iswrite) {
			raw = pl011.pd_ris | PL011_INT_TX;
			*data = raw &
			    pl011.pd_regs[PL011_IMSC / sizeof(uint32_t)];
		}
		break;
	case PL011_ICR:
		if (iswrite) {
			pl011.pd_ris &= ~(*data & PL011_INT_MASK);
			if (pl011.pd_rx_full)
				pl011.pd_ris |= PL011_INT_RX;
			error = pl011_drive_locked();
		}
		break;
	default:
		if (offset > PL011_REG_LAST)
			error = EFAULT;
		else if (iswrite)
			pl011.pd_regs[offset / sizeof(uint32_t)] = *data;
		else
			*data = pl011.pd_regs[offset / sizeof(uint32_t)];
		break;
	}
	mutex_unlock(&pl011.pd_mtx);
	if (resume)
		vm_pipe_send(&pl011.pd_pipe, PL011_RX_RESUME);
	return (error);
}
