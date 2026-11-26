/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-VR-01..04 (T-M7-15): reuse of volume records when the daemon's table (64 records) is full,
 * through the pure rule ntfsmac_vol_reuse_pick() that main.c applies under its lock. A small
 * registry here plays the part of main.c's, driven by a scripted sequence of Disk Arbitration
 * appearances and disappearances (main.c itself needs Disk Arbitration and runs in the guest).
 *   01 after 64 distinct BSD names appeared and went, a 65th disk is handled (gets a record)
 *   02 a record with a live worker, something queued or a request holding it is never reused
 *   03 order: a record whose probe found no NTFS first, then the one that went Gone first
 *   04 nothing reusable: the new disk is not handled (as before), nothing else is touched
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "internal.h"

#define TABLE 64u

typedef struct reg {
	char name[TABLE][32];
	ntfsmac_vol_slot slot[TABLE];
	size_t n;
	uint64_t seq;
} reg;

/* main.c's vol_get_or_create(): the record of `name`, a new one, or a reused one; -1 if none. */
static long appear(reg *r, const char *name)
{
	size_t i;
	long k;

	for (i = 0; i < r->n; i++)
		if (strcmp(r->name[i], name) == 0)
			break;
	if (i == r->n) {
		if (r->n < TABLE) {
			i = r->n++;
		} else {
			k = ntfsmac_vol_reuse_pick(r->slot, r->n);
			if (k < 0)
				return -1;
			i = (size_t)k;
		}
		snprintf(r->name[i], sizeof r->name[i], "%s", name);
		memset(&r->slot[i], 0, sizeof r->slot[i]);
	}
	r->slot[i].state = NTFSMAC_VOL_APPEARED;
	return (long)i;
}

static void gone(reg *r, long i)
{
	r->slot[i].state = NTFSMAC_VOL_GONE;
	r->slot[i].gone_seq = ++r->seq;
}

static void test_full_table(void)
{
	reg r;
	char name[32];
	long k, first;
	unsigned i;

	memset(&r, 0, sizeof r);
	/* 01: 64 names appear and go; the 65th is handled, by the record that went first. */
	for (i = 0; i < TABLE; i++) {
		snprintf(name, sizeof name, "disk%us1", 10 + i);
		k = appear(&r, name);
		CHECK(k == (long)i);
		gone(&r, k);
	}
	first = 0;
	k = appear(&r, "disk99s1");
	CHECK(k == first);
	CHECK(strcmp(r.name[k], "disk99s1") == 0);
	CHECK_EQ_INT(r.n, TABLE);
	/* the old name that comes back later is a new disk too: it takes the next oldest */
	k = appear(&r, "disk10s1");
	CHECK_EQ_INT(k, 1);
}

static void test_never_reused(void)
{
	ntfsmac_vol_slot s[4];

	/* 02: every record is Gone and the oldest is held by something: it is skipped. */
	memset(s, 0, sizeof s);
	for (int i = 0; i < 4; i++) {
		s[i].state = NTFSMAC_VOL_GONE;
		s[i].gone_seq = (uint64_t)(i + 1);
	}
	s[0].worker_live = true; /* a worker not yet reaped */
	s[1].pending = true;     /* an appearance queued, a stray action, or a request */
	CHECK_EQ_INT(ntfsmac_vol_reuse_pick(s, 4), 2);
	s[2].pending = true;
	CHECK_EQ_INT(ntfsmac_vol_reuse_pick(s, 4), 3);
	/* a record that is not Gone, in any state, is never taken */
	for (int st = 0; st < (int)NTFSMAC_VOL_STATE_COUNT_; st++) {
		if (st == (int)NTFSMAC_VOL_GONE)
			continue;
		s[3].state = (ntfsmac_vol_state)st;
		CHECK_EQ_INT(ntfsmac_vol_reuse_pick(s, 4), -1);
	}
	CHECK_EQ_INT(ntfsmac_vol_reuse_pick(NULL, 4), -1);
	CHECK_EQ_INT(ntfsmac_vol_reuse_pick(s, 0), -1);
}

static void test_order(void)
{
	ntfsmac_vol_slot s[3];

	/* 03: the not-NTFS candidate first, even when another went Gone earlier. */
	memset(s, 0, sizeof s);
	s[0].state = s[1].state = s[2].state = NTFSMAC_VOL_GONE;
	s[0].gone_seq = 1;
	s[1].gone_seq = 5;
	s[2].gone_seq = 3;
	CHECK_EQ_INT(ntfsmac_vol_reuse_pick(s, 3), 0);
	s[1].not_ntfs = true;
	s[2].not_ntfs = true;
	CHECK_EQ_INT(ntfsmac_vol_reuse_pick(s, 3), 2);
}

static void test_none(void)
{
	reg r;
	char name[32];
	unsigned i;

	/* 04: 64 volumes present (or busy): the 65th disk is not handled; nothing changes. */
	memset(&r, 0, sizeof r);
	for (i = 0; i < TABLE; i++) {
		snprintf(name, sizeof name, "disk%us1", 10 + i);
		CHECK(appear(&r, name) == (long)i);
	}
	gone(&r, 7);
	r.slot[7].worker_live = true;
	CHECK_EQ_INT(appear(&r, "disk99s1"), -1);
	CHECK(strcmp(r.name[7], "disk17s1") == 0);
}

int main(void)
{
	test_full_table();
	test_never_reused();
	test_order();
	test_none();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
