/* $OpenBSD$ */
/* Public domain.  Exercise index publication on the native host CPUs. */
#include <sys/param.h>
#include <err.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "virtio.h"

#define ROUNDS 70000 /* Includes the 16-bit index wrap. */
static void *ring;
static uint64_t payload[64];
static unsigned int consumed;

static void *
producer(void *arg)
{
	unsigned int n, i;

	(void)arg;
	for (n = 1; n <= ROUNDS; n++) {
		while (__atomic_load_n(&consumed, __ATOMIC_ACQUIRE) != n - 1)
			sched_yield();
		for (i = 0; i < nitems(payload); i++)
			payload[i] = ((uint64_t)n << 32) | i;
		virtio_used_idx(ring, n);
	}
	return (NULL);
}

int
main(void)
{
	pthread_t thread;
	unsigned int n, i;

	/* Both split-ring headers put idx at byte offset two. */
	ring = calloc(1, 8);
	if (ring == NULL || pthread_create(&thread, NULL, producer, NULL) != 0)
		errx(1, "initialize publication test");
	for (n = 1; n <= ROUNDS; n++) {
		while (virtio_avail_idx(ring) != (uint16_t)n)
			sched_yield();
		for (i = 0; i < nitems(payload); i++) {
			if (payload[i] != (((uint64_t)n << 32) | i))
				errx(1, "stale payload at iteration %u, word %u", n, i);
		}
		__atomic_store_n(&consumed, n, __ATOMIC_RELEASE);
	}
	if (pthread_join(thread, NULL) != 0)
		errx(1, "join producer");
	free(ring);
	puts("VIRTQUEUE_ORDER_PASS");
	return (0);
}
