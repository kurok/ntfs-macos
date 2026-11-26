/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-QB-01..03 (T-M7-14): the bounded call onto a serial queue that `status` uses for the
 * journal queue (src/daemon/qbounded.c).
 *   01 a free queue: the job runs and its result comes back
 *   02 a queue held by another job: the call returns within its bound with false, the caller's
 *      copy is unchanged, and the job still runs later on its own copy (ASan sees no use after
 *      free of the caller's stack or of the shared block)
 *   03 bad arguments
 * macOS only (libdispatch); elsewhere the test passes with nothing to do.
 */
#include "check.h"

#if defined(__APPLE__)
#include <stdatomic.h>
#include <time.h>

#include "qbounded.h"

typedef struct peek {
	unsigned in;
	unsigned out;
} peek;

static atomic_int g_ran;

static void job(void *p)
{
	peek *a = p;

	a->out = a->in * 2u;
	atomic_fetch_add(&g_ran, 1);
}

static void hold(void *p)
{
	dispatch_semaphore_t gate = p;

	(void)dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER);
}

static void drain(void *p)
{
	(void)p;
}

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void)
{
	dispatch_queue_t q = dispatch_queue_create("qbounded.test", DISPATCH_QUEUE_SERIAL);
	dispatch_semaphore_t gate = dispatch_semaphore_create(0);
	peek a;
	long long t0, dt;
	int i;

	/* 01 */
	a.in = 21;
	a.out = 0;
	CHECK(ntfsmac_queue_call_bounded(q, &a, sizeof a, job, 1000));
	CHECK_EQ_INT((int)a.out, 42);

	/* 02 */
	atomic_store(&g_ran, 0);
	dispatch_async_f(q, gate, hold);
	a.in = 5;
	a.out = 7;
	t0 = mono_ms();
	CHECK(!ntfsmac_queue_call_bounded(q, &a, sizeof a, job, 300));
	dt = mono_ms() - t0;
	CHECK(dt >= 250 && dt < 1300);
	CHECK_EQ_INT((int)a.out, 7);
	CHECK_EQ_INT(atomic_load(&g_ran), 0);
	dispatch_semaphore_signal(gate);
	for (i = 0; i < 200 && atomic_load(&g_ran) == 0; i++) {
		struct timespec ts = {0, 10 * 1000000L};

		nanosleep(&ts, NULL);
	}
	CHECK_EQ_INT(atomic_load(&g_ran), 1);
	CHECK_EQ_INT((int)a.out, 7);
	/* the queue is usable again */
	CHECK(ntfsmac_queue_call_bounded(q, &a, sizeof a, job, 1000));
	CHECK_EQ_INT((int)a.out, 10);

	/* 03 */
	CHECK(!ntfsmac_queue_call_bounded(NULL, &a, sizeof a, job, 100));
	CHECK(!ntfsmac_queue_call_bounded(q, &a, sizeof a, NULL, 100));
	CHECK(!ntfsmac_queue_call_bounded(q, NULL, sizeof a, job, 100));

	dispatch_sync_f(q, NULL, drain); /* every job has run before the queue goes */
	dispatch_release(gate);
	dispatch_release(q);
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
#else
int main(void)
{
	return 0;
}
#endif
