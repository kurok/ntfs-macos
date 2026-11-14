/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Crash tests (criterion 3.12, U-JRN-*): a child process performs one journal
 * operation and is SIGKILLed at kill point k, for every k the operation
 * passes. The parent then checks that the journal holds the old or the new
 * state, never anything partial, and that a leftover temporary file is
 * ignored and cleaned.
 *
 * Linked against ntfsmac_journal_testhooks, the only build of the library
 * with the kill-point hook. A process kill leaves the page cache intact, so
 * this tests the ordering of the steps, not the durability of the fsync
 * calls (that is the F_FULLFSYNC rule in the library).
 */

#include "jtest.h"

#include "ntfsmac/log.h"

#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>

extern void (*ntfsmac_journal_testhook)(const char *point);

#define MAX_POINTS 128
#define SERIAL 0x5E55101ull
#define OTHER 0x07EA2ull

/* ---- hook ---- */

static int g_target;     /* kill at this 1-based point; 0 = only record */
static int g_count;
static int g_pipe = -1;  /* names of the points go here (counting run) */

static void hook(const char *point)
{
	g_count++;
	if (g_pipe >= 0) {
		size_t n = strlen(point) + 1;

		if (write(g_pipe, point, n) != (ssize_t)n)
			_exit(98);
	}
	if (g_target != 0 && g_count == g_target)
		kill(getpid(), SIGKILL);
}

/* ---- scenarios ---- */

struct scenario {
	const char *name;
	/* Parent, before the child runs: build the starting state. */
	void (*setup)(ntfsmac_journal *j, const char *jdir);
	/* Child, with the hook installed: the operation under test. */
	void (*op)(ntfsmac_journal *j, const char *jdir);
	/* Parent, after the kill, with a handle opened before the child ran
	 * (so nothing cleaned up yet). `point` is the last point reached, or
	 * NULL when the operation ran to the end. */
	void (*verify)(ntfsmac_journal *j, const char *jdir, const char *point);
};

static int in(const char *point, const char *const *set)
{
	for (; *set; set++)
		if (strcmp(point, *set) == 0)
			return 1;
	return 0;
}

/* Points after which the entry file is on its final name. */
static const char *const TMP_WINDOW[] = { "write.tmp_created", "write.tmp_written",
					  "write.tmp_synced", NULL };
static const char *const BEFORE_RENAME[] = { "write.begin", "write.tmp_created",
					     "write.tmp_written", "write.tmp_synced", NULL };

static void check_other_untouched(ntfsmac_journal *j)
{
	ntfsmac_journal_entry g, o = jt_entry(OTHER);
	ntfsmac_journal_status st = ntfsmac_journal_get(j, OTHER, &g);

	if (st == NTFSMAC_JOURNAL_OK)
		CHECK(jt_entry_eq(&o, &g));
}

static int count_cb(const ntfsmac_journal_entry *e, void *ctx)
{
	(void)e;
	(*(unsigned *)ctx)++;
	return 0;
}

/* Common to every verify: the journal reads cleanly, strays are ignored, and
 * reopening removes them. */
static void check_readable_and_cleanable(ntfsmac_journal *j, const char *jdir,
					 unsigned want_entries)
{
	unsigned n = 0, skipped = 99;
	ntfsmac_journal *j2 = NULL;

	CHECK_ST(ntfsmac_journal_list(j, count_cb, &n, &skipped), NTFSMAC_JOURNAL_OK);
	CHECK(skipped == 0);
	CHECK(n == want_entries);
	CHECK_ST(ntfsmac_journal_open(jdir, &j2), NTFSMAC_JOURNAL_OK);
	CHECK(jt_count_names(jdir, jt_is_tmp) == 0);
	n = 0;
	skipped = 99;
	CHECK_ST(ntfsmac_journal_list(j2, count_cb, &n, &skipped), NTFSMAC_JOURNAL_OK);
	CHECK(skipped == 0 && n == want_entries);
	ntfsmac_journal_close(j2);
}

/* --- create --- */

static void setup_other_only(ntfsmac_journal *j, const char *jdir)
{
	ntfsmac_journal_entry o = jt_entry(OTHER);

	(void)jdir;
	CHECK_ST(ntfsmac_journal_create(j, &o), NTFSMAC_JOURNAL_OK);
}

static void op_create(ntfsmac_journal *j, const char *jdir)
{
	ntfsmac_journal_entry e = jt_entry(SERIAL);

	(void)jdir;
	if (ntfsmac_journal_create(j, &e) != NTFSMAC_JOURNAL_OK)
		_exit(97);
}

static void verify_create(ntfsmac_journal *j, const char *jdir, const char *point)
{
	ntfsmac_journal_entry g, e = jt_entry(SERIAL);
	ntfsmac_journal_status st = ntfsmac_journal_get(j, SERIAL, &g);
	int stray = jt_count_names(jdir, jt_is_tmp);

	if (point == NULL || !in(point, BEFORE_RENAME)) {
		CHECK(st == NTFSMAC_JOURNAL_OK && jt_entry_eq(&e, &g)); /* new */
		CHECK(stray == 0);
	} else {
		CHECK(st == NTFSMAC_JOURNAL_ERR_NOT_FOUND);             /* old */
		CHECK(stray == (in(point, TMP_WINDOW) ? 1 : 0));
	}
	check_other_untouched(j);
	check_readable_and_cleanable(j, jdir, st == NTFSMAC_JOURNAL_OK ? 2 : 1);
	/* After a kill the create can simply be repeated. */
	if (st == NTFSMAC_JOURNAL_ERR_NOT_FOUND) {
		ntfsmac_journal *j2 = NULL;

		CHECK_ST(ntfsmac_journal_open(jdir, &j2), NTFSMAC_JOURNAL_OK);
		CHECK_ST(ntfsmac_journal_create(j2, &e), NTFSMAC_JOURNAL_OK);
		ntfsmac_journal_close(j2);
	}
}

/* --- update --- */

static void setup_pending_entry(ntfsmac_journal *j, const char *jdir)
{
	ntfsmac_journal_entry e = jt_entry(SERIAL);

	setup_other_only(j, jdir);
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
}

static void setup_set_entry(ntfsmac_journal *j, const char *jdir)
{
	ntfsmac_journal_entry e = jt_entry(SERIAL);

	setup_pending_entry(j, jdir);
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	e.updated = 1700000100;
	CHECK_ST(ntfsmac_journal_update(j, &e), NTFSMAC_JOURNAL_OK);
}

static void op_marker_set(ntfsmac_journal *j, const char *jdir)
{
	ntfsmac_journal_entry e = jt_entry(SERIAL);

	(void)jdir;
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	e.updated = 1700000100;
	if (ntfsmac_journal_update(j, &e) != NTFSMAC_JOURNAL_OK)
		_exit(97);
}

static void op_marker_failed(ntfsmac_journal *j, const char *jdir)
{
	ntfsmac_journal_entry e = jt_entry(SERIAL);

	(void)jdir;
	e.marker = NTFSMAC_JOURNAL_MARKER_FAILED;
	e.updated = 1700000100;
	if (ntfsmac_journal_update(j, &e) != NTFSMAC_JOURNAL_OK)
		_exit(97);
}

static void op_write_error(ntfsmac_journal *j, const char *jdir)
{
	ntfsmac_journal_entry e = jt_entry(SERIAL);

	(void)jdir;
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	e.state = NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
	e.updated = 1700000200;
	if (ntfsmac_journal_update(j, &e) != NTFSMAC_JOURNAL_OK)
		_exit(97);
}

/* A temporary file left by an earlier crash sits at the name when the update
 * starts (planted after open, so the stray cleanup of open did not see it). */
static void op_marker_set_over_stale_tmp(ntfsmac_journal *j, const char *jdir)
{
	char p[PATH_MAX_T];

	jt_path(p, sizeof(p), jdir, ".0000000005E55101.tmp", NULL);
	{
		int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);

		if (fd < 0 || write(fd, "stale", 5) != 5)
			_exit(96);
		close(fd);
	}
	op_marker_set(j, jdir);
}

struct upd_expect {
	ntfsmac_journal_marker old_marker, new_marker;
	ntfsmac_journal_state old_state, new_state;
	int64_t old_updated, new_updated;
};

static void verify_update_generic(ntfsmac_journal *j, const char *jdir,
				  const char *point, const struct upd_expect *x)
{
	ntfsmac_journal_entry g;
	ntfsmac_journal_status st = ntfsmac_journal_get(j, SERIAL, &g);
	int stray = jt_count_names(jdir, jt_is_tmp);
	int is_new = point == NULL || !in(point, BEFORE_RENAME);

	CHECK(st == NTFSMAC_JOURNAL_OK); /* never missing, never corrupt */
	CHECK(g.serial == SERIAL && strcmp(g.label, "TESTVOL") == 0);
	if (is_new) {
		CHECK(g.marker == x->new_marker && g.state == x->new_state &&
		      g.updated == x->new_updated);
		CHECK(stray == 0);
	} else {
		CHECK(g.marker == x->old_marker && g.state == x->old_state &&
		      g.updated == x->old_updated);
		CHECK(stray <= 1);
		/* Not before the temporary exists; stale-tmp scenario plants
		 * one earlier, so only the upper bound is fixed here. */
	}
	check_other_untouched(j);
	check_readable_and_cleanable(j, jdir, 2);
}

static void verify_marker_set(ntfsmac_journal *j, const char *jdir, const char *point)
{
	struct upd_expect x = { NTFSMAC_JOURNAL_MARKER_PENDING, NTFSMAC_JOURNAL_MARKER_SET,
				NTFSMAC_JOURNAL_STATE_OPEN, NTFSMAC_JOURNAL_STATE_OPEN,
				1700000000, 1700000100 };

	verify_update_generic(j, jdir, point, &x);
}

static void verify_marker_failed(ntfsmac_journal *j, const char *jdir, const char *point)
{
	struct upd_expect x = { NTFSMAC_JOURNAL_MARKER_PENDING, NTFSMAC_JOURNAL_MARKER_FAILED,
				NTFSMAC_JOURNAL_STATE_OPEN, NTFSMAC_JOURNAL_STATE_OPEN,
				1700000000, 1700000100 };

	verify_update_generic(j, jdir, point, &x);
}

static void verify_write_error(ntfsmac_journal *j, const char *jdir, const char *point)
{
	struct upd_expect x = { NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_MARKER_SET,
				NTFSMAC_JOURNAL_STATE_OPEN, NTFSMAC_JOURNAL_STATE_WRITE_ERROR,
				1700000100, 1700000200 };

	verify_update_generic(j, jdir, point, &x);
}

/* --- removal --- */

#define T_REMOVE 1800000000ll
#define T_OLD 1700000000ll

static void setup_remove_few(ntfsmac_journal *j, const char *jdir)
{
	ntfsmac_journal_entry e = jt_entry(SERIAL);

	setup_other_only(j, jdir);
	e.marker = NTFSMAC_JOURNAL_MARKER_NONE;
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
}

/* 32 tombstones already (serials 1000..1031), so the removal prunes one. */
static void setup_remove_full(ntfsmac_journal *j, const char *jdir)
{
	setup_remove_few(j, jdir);
	for (int i = 0; i < 32; i++) {
		ntfsmac_journal_entry e = jt_entry(1000 + (uint64_t)i);

		e.marker = NTFSMAC_JOURNAL_MARKER_NONE;
		CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
		CHECK_ST(ntfsmac_journal_remove(j, e.serial,
						NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION,
						T_OLD + i),
			 NTFSMAC_JOURNAL_OK);
	}
}

static void op_remove(ntfsmac_journal *j, const char *jdir)
{
	(void)jdir;
	if (ntfsmac_journal_remove(j, SERIAL, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION,
				   T_REMOVE) != NTFSMAC_JOURNAL_OK)
		_exit(97);
}

struct tcount {
	unsigned n;
	int has_new, has_oldest, has_second;
};

static int tomb_cb(uint64_t serial, int64_t when, void *ctx)
{
	struct tcount *c = ctx;

	c->n++;
	if (serial == SERIAL && when == T_REMOVE)
		c->has_new = 1;
	if (serial == 1000 && when == T_OLD)
		c->has_oldest = 1;
	if (serial == 1001 && when == T_OLD + 1)
		c->has_second = 1;
	return 0;
}

static void verify_remove_common(ntfsmac_journal *j, const char *jdir,
				 const char *point, unsigned base_tombs, int expect_prune)
{
	ntfsmac_journal_entry g;
	ntfsmac_journal_status st = ntfsmac_journal_get(j, SERIAL, &g);
	struct tcount t;
	int renamed = point == NULL || strcmp(point, "remove.begin") != 0;

	memset(&t, 0, sizeof(t));
	CHECK_ST(ntfsmac_journal_list_tombstones(j, tomb_cb, &t), NTFSMAC_JOURNAL_OK);
	/* The entry is in exactly one place: the journal, or cleared/. */
	if (!renamed) {
		CHECK(st == NTFSMAC_JOURNAL_OK && !t.has_new);
		CHECK(t.n == base_tombs);
	} else {
		CHECK(st == NTFSMAC_JOURNAL_ERR_NOT_FOUND && t.has_new);
		if (expect_prune) {
			int pruned = point == NULL || strcmp(point, "remove.pruned") == 0 ||
				     strcmp(point, "remove.prune_synced") == 0 ||
				     strcmp(point, "remove.done") == 0;

			/* 32 old + new: 33 until the oldest is unlinked, then 32. */
			CHECK(t.n == (pruned ? 32u : 33u));
			CHECK(t.has_oldest == !pruned);
			CHECK(t.has_second); /* only the oldest goes */
		} else {
			CHECK(t.n == base_tombs + 1);
		}
	}
	CHECK(jt_count_names(jdir, jt_is_tmp) == 0);
	check_other_untouched(j);
	check_readable_and_cleanable(j, jdir, renamed ? 1 : 2);
}

static void verify_remove_few(ntfsmac_journal *j, const char *jdir, const char *point)
{
	verify_remove_common(j, jdir, point, 0, 0);
}

static void verify_remove_full(ntfsmac_journal *j, const char *jdir, const char *point)
{
	verify_remove_common(j, jdir, point, 32, 1);
}

/* ---- driver ---- */

static const struct scenario SCENARIOS[] = {
	{ "create", setup_other_only, op_create, verify_create },
	{ "update marker pending->set", setup_pending_entry, op_marker_set, verify_marker_set },
	{ "update marker pending->failed", setup_pending_entry, op_marker_failed, verify_marker_failed },
	{ "update state open->write_error", setup_set_entry, op_write_error, verify_write_error },
	{ "update over stale temporary", setup_pending_entry, op_marker_set_over_stale_tmp, verify_marker_set },
	{ "remove (few tombstones)", setup_remove_few, op_remove, verify_remove_few },
	{ "remove (prunes the oldest of 32)", setup_remove_full, op_remove, verify_remove_full },
};

/* Fork a child that opens the journal, installs the hook and runs the op.
 * Returns the wait status; names (if non-NULL) collects the points passed. */
static int run_child(const struct scenario *sc, const char *jdir, int target,
		     char names[][40], int *npoints)
{
	int fds[2] = { -1, -1 };
	pid_t pid;
	int status;

	if (names != NULL)
		CHECK(pipe(fds) == 0);
	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		ntfsmac_journal *j = NULL;

		if (names != NULL) {
			close(fds[0]);
			g_pipe = fds[1];
		}
		if (ntfsmac_journal_open(jdir, &j) != NTFSMAC_JOURNAL_OK)
			_exit(99);
		g_target = target;
		g_count = 0;
		ntfsmac_journal_testhook = hook;
		sc->op(j, jdir);
		ntfsmac_journal_testhook = NULL;
		ntfsmac_journal_close(j);
		_exit(0);
	}
	if (names != NULL) {
		char buf[40 * MAX_POINTS];
		size_t got = 0;
		int n = 0;

		close(fds[1]);
		for (;;) {
			ssize_t r = read(fds[0], buf + got, sizeof(buf) - got);

			if (r < 0 && errno == EINTR)
				continue;
			CHECK(r >= 0);
			if (r == 0)
				break;
			got += (size_t)r;
		}
		close(fds[0]);
		for (size_t i = 0; i < got;) {
			size_t l = strlen(buf + i) + 1;

			CHECK(n < MAX_POINTS && l <= 40);
			memcpy(names[n++], buf + i, l);
			i += l;
		}
		*npoints = n;
	}
	CHECK(waitpid(pid, &status, 0) == pid);
	return status;
}

static void fresh(const struct scenario *sc, char *base, size_t bsize, char *jdir,
		  size_t jsize, ntfsmac_journal **jp)
{
	jt_jdir(base, bsize, jdir, jsize);
	CHECK_ST(ntfsmac_journal_open(jdir, jp), NTFSMAC_JOURNAL_OK);
	sc->setup(*jp, jdir);
}

int main(void)
{
	static char names[MAX_POINTS][40];
	int total_kills = 0;

	ntfsmac_log_set_level(NTFSMAC_LOG_NONE);
	for (size_t s = 0; s < sizeof(SCENARIOS) / sizeof(SCENARIOS[0]); s++) {
		const struct scenario *sc = &SCENARIOS[s];
		char base[PATH_MAX_T], jdir[PATH_MAX_T];
		ntfsmac_journal *j;
		int npoints = 0, status;

		/* Counting run: uninterrupted, records the kill points. */
		fresh(sc, base, sizeof(base), jdir, sizeof(jdir), &j);
		status = run_child(sc, jdir, 0, names, &npoints);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		CHECK(npoints >= 4 && npoints < MAX_POINTS);
		sc->verify(j, jdir, NULL);
		ntfsmac_journal_close(j);
		jt_rmrf(base);

		for (int k = 1; k <= npoints; k++) {
			fresh(sc, base, sizeof(base), jdir, sizeof(jdir), &j);
			status = run_child(sc, jdir, k, NULL, NULL);
			if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)) {
				fprintf(stderr, "FAIL: %s: child not killed at point %d (%s), status %d\n",
					sc->name, k, names[k - 1], status);
				return 1;
			}
			sc->verify(j, jdir, names[k - 1]);
			ntfsmac_journal_close(j);
			jt_rmrf(base);
			total_kills++;
		}
		printf("journal.crash: %-36s %2d kill points, all consistent\n", sc->name, npoints);
	}
	printf("journal.crash: %d kills in %zu scenarios\n", total_kills,
	       sizeof(SCENARIOS) / sizeof(SCENARIOS[0]));
	return 0;
}
