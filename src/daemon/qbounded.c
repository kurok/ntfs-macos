/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A synchronous call onto a serial dispatch queue with a time bound (qbounded.h, T-M7-14). The
 * job and the caller share one block; whichever of the two finishes with it last frees it.
 */
#include "qbounded.h"

#if defined(__APPLE__)

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct qcall {
	pthread_mutex_t mu;
	int refs; /* the job and the caller */
	dispatch_semaphore_t done;
	void (*fn)(void *);
	size_t size;
	unsigned char data[];
} qcall;

static void qcall_unref(qcall *c)
{
	int left;

	pthread_mutex_lock(&c->mu);
	left = --c->refs;
	pthread_mutex_unlock(&c->mu);
	if (left != 0)
		return;
	dispatch_release(c->done);
	pthread_mutex_destroy(&c->mu);
	free(c);
}

static void qcall_run(void *p)
{
	qcall *c = p;

	c->fn(c->data);
	dispatch_semaphore_signal(c->done);
	qcall_unref(c);
}

bool ntfsmac_queue_call_bounded(dispatch_queue_t q, void *arg, size_t size, void (*fn)(void *),
				unsigned ms)
{
	qcall *c;
	bool ok;

	if (q == NULL || fn == NULL || (size > 0 && arg == NULL))
		return false;
	c = calloc(1, sizeof *c + size);
	if (c == NULL)
		return false;
	c->done = dispatch_semaphore_create(0);
	if (c->done == NULL || pthread_mutex_init(&c->mu, NULL) != 0) {
		if (c->done != NULL)
			dispatch_release(c->done);
		free(c);
		return false;
	}
	c->refs = 2;
	c->fn = fn;
	c->size = size;
	if (size > 0)
		memcpy(c->data, arg, size);
	dispatch_async_f(q, c, qcall_run);
	ok = dispatch_semaphore_wait(c->done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC)) == 0;
	if (ok && size > 0) {
		/* The job signalled after its last write to the copy, so the copy is complete. */
		memcpy(arg, c->data, size);
	}
	qcall_unref(c);
	return ok;
}

#else

/* No libdispatch: nothing to build (the daemon is macOS only). */
typedef int ntfsmac_qbounded_unused;

#endif
