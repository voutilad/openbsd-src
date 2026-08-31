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

#include <sys/time.h>
#include <sys/types.h>

#include <machine/armreg.h>

#include <errno.h>
#include <event.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "vmd.h"
#include "arm64_timer.h"
#include "gicv3.h"

#define CNTP_CTL_ENABLE		(1U << 0)
#define CNTP_CTL_IMASK		(1U << 1)
#define CNTP_CTL_ISTATUS	(1U << 2)

struct arm64_timer_dev {
	pthread_mutex_t	 td_mtx;
	uint32_t	 td_vm_id;
	uint32_t	 td_ctl;
	struct timespec	 td_deadline;
	int		 td_deadline_valid;
	int		 td_expired;
	int		 td_irq_line;
	int		 td_paused;
	struct event	 td_event;
	struct vm_dev_pipe td_pipe;
};

static struct arm64_timer_dev arm64_timer;

static void	arm64_timer_fire(int, short, void *);
static int	arm64_timer_is_sysreg(uint64_t, int);
static void	arm64_timer_pipe_dispatch(int, short, void *);
static int	arm64_timer_refresh_locked(void);
static void	arm64_timer_schedule_locked(void);
static int32_t	arm64_timer_tval_locked(void);

static int
arm64_timer_is_sysreg(uint64_t esr, int op2)
{
	return (ISS_MSR_OP0(esr) == 3 && ISS_MSR_OP1(esr) == 3 &&
	    ISS_MSR_CRn(esr) == 14 && ISS_MSR_CRm(esr) == 2 &&
	    ISS_MSR_OP2(esr) == op2);
}

static int
arm64_timer_refresh_locked(void)
{
	struct timespec now;
	int irq_line;

	if ((arm64_timer.td_ctl & CNTP_CTL_ENABLE) != 0 &&
	    arm64_timer.td_deadline_valid && !arm64_timer.td_expired) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (timespeccmp(&now, &arm64_timer.td_deadline, >=))
			arm64_timer.td_expired = 1;
	}
	irq_line = (arm64_timer.td_ctl & CNTP_CTL_ENABLE) != 0 &&
	    (arm64_timer.td_ctl & CNTP_CTL_IMASK) == 0 &&
	    arm64_timer.td_expired;
	if (irq_line == arm64_timer.td_irq_line)
		return (0);
	if (gicv3_set_irq(arm64_timer.td_vm_id, 0, ARM64_TIMER_INTID,
	    irq_line) != 0)
		return (EIO);
	arm64_timer.td_irq_line = irq_line;
	return (0);
}

static int32_t
arm64_timer_tval_locked(void)
{
	struct timespec delta, now;
	uint64_t ticks;

	if (!arm64_timer.td_deadline_valid)
		return (0);
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (timespeccmp(&now, &arm64_timer.td_deadline, >=))
		return (0);
	timespecsub(&arm64_timer.td_deadline, &now, &delta);
	ticks = (uint64_t)delta.tv_sec * ARM64_TIMER_FREQUENCY;
	ticks += (uint64_t)delta.tv_nsec * ARM64_TIMER_FREQUENCY /
	    1000000000ULL;
	if (ticks > INT32_MAX)
		return (INT32_MAX);
	return (ticks);
}

static void
arm64_timer_schedule_locked(void)
{
	struct timespec delta, now;
	struct timeval timeout;

	evtimer_del(&arm64_timer.td_event);
	if (arm64_timer.td_paused ||
	    (arm64_timer.td_ctl & CNTP_CTL_ENABLE) == 0 ||
	    !arm64_timer.td_deadline_valid || arm64_timer.td_expired)
		return;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (timespeccmp(&now, &arm64_timer.td_deadline, >=)) {
		(void)arm64_timer_refresh_locked();
		return;
	}
	timespecsub(&arm64_timer.td_deadline, &now, &delta);
	timeout.tv_sec = delta.tv_sec;
	timeout.tv_usec = (delta.tv_nsec + 999) / 1000;
	if (timeout.tv_usec >= 1000000) {
		timeout.tv_sec++;
		timeout.tv_usec -= 1000000;
	}
	evtimer_add(&arm64_timer.td_event, &timeout);
}

static void
arm64_timer_fire(int fd, short event, void *arg)
{
	(void)fd;
	(void)event;
	(void)arg;

	mutex_lock(&arm64_timer.td_mtx);
	if (arm64_timer_refresh_locked() != 0)
		log_warnx("failed to assert arm64 timer interrupt");
	mutex_unlock(&arm64_timer.td_mtx);
}

static void
arm64_timer_pipe_dispatch(int fd, short event, void *arg)
{
	enum pipe_msg_type msg;

	(void)fd;
	(void)event;
	(void)arg;

	msg = vm_pipe_recv(&arm64_timer.td_pipe);
	if (msg != ARM64_TIMER_RESCHEDULE)
		fatalx("%s: unexpected pipe message %d", __func__, msg);
	mutex_lock(&arm64_timer.td_mtx);
	arm64_timer_schedule_locked();
	mutex_unlock(&arm64_timer.td_mtx);
}

void
arm64_timer_init(uint32_t vm_id)
{
	int error;

	memset(&arm64_timer, 0, sizeof(arm64_timer));
	error = pthread_mutex_init(&arm64_timer.td_mtx, NULL);
	if (error != 0) {
		errno = error;
		fatal("could not initialize arm64 timer mutex");
	}
	arm64_timer.td_vm_id = vm_id;
	evtimer_set(&arm64_timer.td_event, arm64_timer_fire, NULL);
	vm_pipe_init(&arm64_timer.td_pipe, arm64_timer_pipe_dispatch);
	event_add(&arm64_timer.td_pipe.read_ev, NULL);
}

void
arm64_timer_pause(void)
{
	mutex_lock(&arm64_timer.td_mtx);
	arm64_timer.td_paused = 1;
	evtimer_del(&arm64_timer.td_event);
	mutex_unlock(&arm64_timer.td_mtx);
}

void
arm64_timer_unpause(void)
{
	mutex_lock(&arm64_timer.td_mtx);
	arm64_timer.td_paused = 0;
	arm64_timer_schedule_locked();
	mutex_unlock(&arm64_timer.td_mtx);
}

int
arm64_timer_sysreg(uint64_t esr, int write, uint64_t *data)
{
	struct timespec now;
	int32_t tval;
	uint64_t nsec;
	int error = 0, reschedule = 0;

	if (data == NULL)
		return (EINVAL);
	if (!arm64_timer_is_sysreg(esr, 0) &&
	    !arm64_timer_is_sysreg(esr, 1))
		return (ENOENT);

	mutex_lock(&arm64_timer.td_mtx);
	if (arm64_timer_is_sysreg(esr, 0)) { /* CNTP_TVAL_EL0 */
		if (write) {
			tval = (uint32_t)*data;
			clock_gettime(CLOCK_MONOTONIC, &now);
			arm64_timer.td_deadline = now;
			if (tval > 0) {
				arm64_timer.td_deadline.tv_sec +=
				    tval / ARM64_TIMER_FREQUENCY;
				nsec = (uint64_t)(tval % ARM64_TIMER_FREQUENCY) *
				    1000000000ULL / ARM64_TIMER_FREQUENCY;
				arm64_timer.td_deadline.tv_nsec += nsec;
				if (arm64_timer.td_deadline.tv_nsec >=
				    1000000000L) {
					arm64_timer.td_deadline.tv_sec++;
					arm64_timer.td_deadline.tv_nsec -=
					    1000000000L;
				}
			}
			arm64_timer.td_deadline_valid = 1;
			arm64_timer.td_expired = tval <= 0;
			reschedule = 1;
		} else
			*data = (uint32_t)arm64_timer_tval_locked();
	} else { /* CNTP_CTL_EL0 */
		if (write) {
			arm64_timer.td_ctl = *data &
			    (CNTP_CTL_ENABLE | CNTP_CTL_IMASK);
			reschedule = 1;
		} else {
			error = arm64_timer_refresh_locked();
			*data = arm64_timer.td_ctl;
			if ((arm64_timer.td_ctl & CNTP_CTL_ENABLE) != 0 &&
			    arm64_timer.td_expired)
				*data |= CNTP_CTL_ISTATUS;
		}
	}
	if (write)
		error = arm64_timer_refresh_locked();
	mutex_unlock(&arm64_timer.td_mtx);
	if (reschedule)
		vm_pipe_send(&arm64_timer.td_pipe, ARM64_TIMER_RESCHEDULE);
	return (error);
}
