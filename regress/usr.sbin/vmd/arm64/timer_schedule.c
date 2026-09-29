/*	$OpenBSD$ */
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

#include <err.h>

/* Include the implementation to control the deadline/event handoff race. */
#include "arm64_timer.c"

static int irq_calls, wake_calls, signal_calls;

int
gicv3_set_irq(int fd, uint32_t cpu, int intid, int level)
{
	if (fd != 42 || cpu != 0 || intid != ARM64_TIMER_INTID || level != 1)
		errx(1, "unexpected software timer interrupt");
	irq_calls++;
	return (0);
}

void
vcpu_unhalt(uint32_t cpu)
{
	if (cpu != 0)
		errx(1, "unexpected wakeup CPU");
	wake_calls++;
}

void
vcpu_signal_run(uint32_t cpu)
{
	if (cpu != 0)
		errx(1, "unexpected signal CPU");
	signal_calls++;
}

void
mutex_lock(pthread_mutex_t *m)
{
	if (pthread_mutex_lock(m) != 0)
		errx(1, "mutex lock");
}

void
mutex_unlock(pthread_mutex_t *m)
{
	if (pthread_mutex_unlock(m) != 0)
		errx(1, "mutex unlock");
}

/* This unit test enters at the event thread's reschedule operation. */
void
vm_pipe_init(struct vm_dev_pipe *p, void (*cb)(int, short, void *))
{
	(void)p;
	(void)cb;
	errx(1, "unexpected pipe init");
}

void
vm_pipe_send(struct vm_dev_pipe *p, enum pipe_msg_type msg)
{
	(void)p;
	(void)msg;
	errx(1, "unexpected pipe send");
}

enum pipe_msg_type
vm_pipe_recv(struct vm_dev_pipe *p)
{
	(void)p;
	errx(1, "unexpected pipe receive");
}

static void
expired_deadline(int hardware, int masked)
{
	irq_calls = wake_calls = signal_calls = 0;
	arm64_timer.td_vm_fd = 42;
	arm64_timer.td_hardware = hardware;
	arm64_timer.td_ctl = CNTV_CTL_ENABLE | (masked ? CNTV_CTL_IMASK : 0);
	arm64_timer.td_deadline_valid = 1;
	arm64_timer.td_expired = arm64_timer.td_irq_line = 0;
	clock_gettime(CLOCK_MONOTONIC, &arm64_timer.td_deadline);
	arm64_timer.td_deadline.tv_sec--;

	/* The vCPU saw a future deadline, but it expired before event_add. */
	arm64_timer_schedule_locked();
	if (irq_calls != 0)
		errx(1, "late scheduling injected an IRQ outside the timer callback");
	if (hardware && masked) {
		if (evtimer_pending(&arm64_timer.td_event, NULL))
			errx(1, "masked hardware timer scheduled a wakeup");
		return;
	}
	if (!evtimer_pending(&arm64_timer.td_event, NULL))
		errx(1, "late scheduling lost the wakeup");
	event_loop(EVLOOP_ONCE);
	if (irq_calls != !hardware || wake_calls != hardware ||
	    signal_calls != hardware || !arm64_timer.td_expired)
		errx(1, "wrong hardware/software timer delivery path");
}

int
main(void)
{
	event_init();
	if (pthread_mutex_init(&arm64_timer.td_mtx, NULL) != 0)
		errx(1, "mutex init");
	evtimer_set(&arm64_timer.td_event, arm64_timer_fire, NULL);
	expired_deadline(1, 0);
	expired_deadline(1, 1);
	expired_deadline(0, 0);
	puts("late timer scheduling preserves hardware-only IRQ delivery");
	return (0);
}
