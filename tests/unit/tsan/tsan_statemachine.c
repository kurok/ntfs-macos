/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-TSAN-01: the volume state machine driven from several threads with random
 * events.
 *
 * ARCHITECTURE.md section 5: one serial queue per volume, and "no shared
 * mutable state between volumes". volume.c has no lock because one volume
 * record belongs to one queue. This driver models exactly that:
 *
 *   - NVOL volumes, each with a queue (mutex, condition variables, ring) and
 *     one consumer thread. Only that thread touches the volume record and its
 *     event log until it is joined. The records sit next to each other in one
 *     array, so a hidden global in volume.c, or a record that two queues
 *     touch, would be seen by ThreadSanitizer.
 *   - NPROD producer threads (the main queue: Disk Arbitration events and
 *     control requests) post random events to random volumes.
 *
 * After every event the consumer checks the record invariants. At the end the
 * main thread replays each volume's events on a fresh record in one thread and
 * compares: the final record must be the same, so no other volume and no
 * thread timing changed the result.
 *
 * Run it with the `tsan` preset (cmake --preset tsan) to get the race check;
 * it also runs in the normal build as a plain concurrency test.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "internal.h"

#define NVOL 8
#define NPROD 4
#define EVENTS_PER_PRODUCER 40000
#define QCAP 128

typedef struct qitem {
	ntfsmac_vol_event ev;
	ntfsmac_vol_event_arg arg;
	bool has_arg;
} qitem;

typedef struct vqueue {
	pthread_mutex_t mu;
	pthread_cond_t not_empty, not_full;
	qitem item[QCAP];
	unsigned head, count;
	bool closed;
} vqueue;

typedef struct logrec {
	qitem q;
	ntfsmac_vol_result res;
} logrec;

typedef struct vol_slot {
	vqueue q;
	/* Touched only by this slot's consumer thread until it is joined. */
	ntfsmac_volume v;
	logrec *log;
	size_t nlog, caplog;
	unsigned accepted, refused;
	pthread_t tid;
} vol_slot;

static vol_slot g_slot[NVOL];

static void q_init(vqueue *q)
{
	memset(q, 0, sizeof *q);
	pthread_mutex_init(&q->mu, NULL);
	pthread_cond_init(&q->not_empty, NULL);
	pthread_cond_init(&q->not_full, NULL);
}

static void q_put(vqueue *q, const qitem *it)
{
	pthread_mutex_lock(&q->mu);
	while (q->count == QCAP)
		pthread_cond_wait(&q->not_full, &q->mu);
	q->item[(q->head + q->count) % QCAP] = *it;
	q->count++;
	pthread_cond_signal(&q->not_empty);
	pthread_mutex_unlock(&q->mu);
}

static void q_close(vqueue *q)
{
	pthread_mutex_lock(&q->mu);
	q->closed = true;
	pthread_cond_broadcast(&q->not_empty);
	pthread_mutex_unlock(&q->mu);
}

/* false when the queue is closed and empty */
static bool q_get(vqueue *q, qitem *out)
{
	bool ok = false;

	pthread_mutex_lock(&q->mu);
	while (q->count == 0 && !q->closed)
		pthread_cond_wait(&q->not_empty, &q->mu);
	if (q->count > 0) {
		*out = q->item[q->head];
		q->head = (q->head + 1) % QCAP;
		q->count--;
		pthread_cond_signal(&q->not_full);
		ok = true;
	}
	pthread_mutex_unlock(&q->mu);
	return ok;
}

static uint64_t rnd_next(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*s = x;
	return x;
}

static bool same_record(const ntfsmac_volume *a, const ntfsmac_volume *b)
{
	return a->state == b->state && a->planned_mode == b->planned_mode && a->mode == b->mode &&
	       a->reason == b->reason && a->disk_present == b->disk_present;
}

/* The record invariants that hold after every event, accepted or not. */
static void check_invariants(const ntfsmac_volume *v)
{
	bool mounted_side = v->state == NTFSMAC_VOL_MOUNTED || v->state == NTFSMAC_VOL_UNMOUNTING;

	CHECK((unsigned)v->state < (unsigned)NTFSMAC_VOL_STATE_COUNT_);
	/* a real mode exists only where a mount exists or is being cleaned up */
	if (v->mode != NTFSMAC_VMODE_NONE)
		CHECK(mounted_side);
	if (v->planned_mode != NTFSMAC_VMODE_NONE)
		CHECK(v->state == NTFSMAC_VOL_MOUNTING || mounted_side);
	/* a ro decision never ends in a writable mount */
	if (v->planned_mode == NTFSMAC_VMODE_RO)
		CHECK(v->mode != NTFSMAC_VMODE_RW);
	/* Failed always says why; Appeared and Gone say nothing */
	if (v->state == NTFSMAC_VOL_FAILED)
		CHECK(v->reason != NTFSMAC_VREASON_NONE);
	if (v->state == NTFSMAC_VOL_APPEARED || v->state == NTFSMAC_VOL_GONE) {
		CHECK(v->reason == NTFSMAC_VREASON_NONE);
		CHECK(v->mode == NTFSMAC_VMODE_NONE && v->planned_mode == NTFSMAC_VMODE_NONE);
	}
	/* a lost disk leaves only Unmounting (cleanup) or Gone */
	if (!v->disk_present)
		CHECK(v->state == NTFSMAC_VOL_UNMOUNTING || v->state == NTFSMAC_VOL_GONE);
}

static void *consumer_main(void *arg)
{
	vol_slot *s = arg;
	qitem it;

	while (q_get(&s->q, &it)) {
		ntfsmac_volume before = s->v;
		ntfsmac_vol_state want;
		ntfsmac_vol_result r;
		logrec *lr;

		want = ntfsmac_vol_next_state(before.state, it.ev, before.disk_present);
		r = ntfsmac_volume_apply(&s->v, it.ev, it.has_arg ? &it.arg : NULL);

		if (r == NTFSMAC_VRES_OK) {
			s->accepted++;
			CHECK(s->v.state == want);
			CHECK(before.state != NTFSMAC_VOL_GONE); /* Gone accepts nothing */
			if (!before.disk_present)
				CHECK(!s->v.disk_present); /* the loss is permanent */
		} else {
			s->refused++;
			CHECK(same_record(&before, &s->v)); /* a refusal changes nothing */
		}
		check_invariants(&s->v);

		if (s->nlog == s->caplog) {
			s->caplog = s->caplog ? s->caplog * 2 : 1024;
			s->log = realloc(s->log, s->caplog * sizeof *s->log);
			if (s->log == NULL) {
				fprintf(stderr, "out of memory\n");
				exit(2);
			}
		}
		lr = &s->log[s->nlog++];
		lr->q = it;
		lr->res = r;
	}
	return NULL;
}

/* Pick an event. Disk loss is rare so that volumes live long enough to be tested. */
static void random_event(uint64_t *rs, qitem *it)
{
	uint64_t r = rnd_next(rs);
	unsigned pick = (unsigned)(r % 4000u);

	memset(it, 0, sizeof *it);
	if (pick == 0) {
		it->ev = NTFSMAC_VEV_DISK_GONE;
	} else if (pick < 1000) {
		it->ev = NTFSMAC_VEV_START;
	} else {
		it->ev = (ntfsmac_vol_event)((r >> 8) % (uint64_t)NTFSMAC_VEV_COUNT_);
		if (it->ev == NTFSMAC_VEV_DISK_GONE)
			it->ev = NTFSMAC_VEV_START;
	}
	/* Arguments: mostly valid modes and reasons, sometimes none, sometimes bad. */
	switch ((r >> 20) % 8u) {
	case 0:
		it->has_arg = false;
		break;
	case 1:
		it->has_arg = true;
		it->arg.mode = NTFSMAC_VMODE_NONE;
		it->arg.reason = NTFSMAC_VREASON_NONE;
		break;
	case 2:
		it->has_arg = true;
		it->arg.mode = (ntfsmac_vol_mode)99;
		it->arg.reason = (ntfsmac_vol_reason)((r >> 30) % 4u);
		break;
	default:
		it->has_arg = true;
		it->arg.mode = (r >> 24) & 1u ? NTFSMAC_VMODE_RW : NTFSMAC_VMODE_RO;
		it->arg.reason = (ntfsmac_vol_reason)((r >> 32) % (uint64_t)NTFSMAC_VREASON_COUNT_);
		break;
	}
}

static void *producer_main(void *arg)
{
	uint64_t rs = 0x9E3779B97F4A7C15ull * ((uintptr_t)arg + 1u);
	int i;

	for (i = 0; i < EVENTS_PER_PRODUCER; i++) {
		qitem it;
		unsigned vol = (unsigned)(rnd_next(&rs) % NVOL);

		random_event(&rs, &it);
		q_put(&g_slot[vol].q, &it);
	}
	return NULL;
}

/* One round: fresh volumes, producers, consumers, then the single-thread replay. */
static void run_round(unsigned *total_ok, unsigned *total_refused, unsigned *gone)
{
	pthread_t prod[NPROD];
	size_t i, k;

	for (i = 0; i < NVOL; i++) {
		vol_slot *s = &g_slot[i];

		memset(s, 0, sizeof *s);
		q_init(&s->q);
		ntfsmac_volume_init(&s->v);
	}
	for (i = 0; i < NVOL; i++)
		CHECK(pthread_create(&g_slot[i].tid, NULL, consumer_main, &g_slot[i]) == 0);
	for (i = 0; i < NPROD; i++)
		CHECK(pthread_create(&prod[i], NULL, producer_main, (void *)(uintptr_t)i) == 0);
	for (i = 0; i < NPROD; i++)
		pthread_join(prod[i], NULL);
	for (i = 0; i < NVOL; i++)
		q_close(&g_slot[i].q);
	for (i = 0; i < NVOL; i++)
		pthread_join(g_slot[i].tid, NULL);

	/* Single-thread replay of each volume's own queue order. */
	for (i = 0; i < NVOL; i++) {
		vol_slot *s = &g_slot[i];
		ntfsmac_volume ref;

		ntfsmac_volume_init(&ref);
		for (k = 0; k < s->nlog; k++) {
			const logrec *lr = &s->log[k];
			ntfsmac_vol_result r = ntfsmac_volume_apply(
				&ref, lr->q.ev, lr->q.has_arg ? &lr->q.arg : NULL);

			CHECK(r == lr->res);
		}
		CHECK(same_record(&ref, &s->v));
		CHECK(s->nlog == (size_t)s->accepted + s->refused);
		*total_ok += s->accepted;
		*total_refused += s->refused;
		if (s->v.state == NTFSMAC_VOL_GONE)
			(*gone)++;
		free(s->log);
		pthread_mutex_destroy(&s->q.mu);
		pthread_cond_destroy(&s->q.not_empty);
		pthread_cond_destroy(&s->q.not_full);
	}
}

int main(void)
{
	unsigned ok = 0, refused = 0, gone = 0;
	int round;

	for (round = 0; round < 3; round++)
		run_round(&ok, &refused, &gone);

	/* The run must have exercised the machine, not only been refused. */
	CHECK(ok > 10000);
	CHECK(refused > 10000);
	printf("tsan_statemachine: %u events accepted, %u refused, %u volumes ended Gone\n", ok,
	       refused, gone);
	if (check_failures() != 0) {
		fprintf(stderr, "%d check(s) failed\n", check_failures());
		return 1;
	}
	return 0;
}
