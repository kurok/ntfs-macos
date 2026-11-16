/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-EW-01..07: session channel parser, clean-session proof and pipe
 * (src/daemon/errwatch.c, errwatch_io.c; ARCHITECTURE.md section 7, ADR-018).
 * Host test: no worker, no device. The real worker is covered by
 * tests/integration/daemon/errwatch_vm.sh.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "internal.h"

static ntfsmac_dmn_status feed_str(ntfsmac_errwatch *w, const char *s)
{
	return ntfsmac_errwatch_feed(w, s, strlen(s));
}

/* Parse `s` and reach end-of-file; return the proof for a rw session in the good case. */
static bool proof_of(const char *s)
{
	ntfsmac_errwatch w;

	ntfsmac_errwatch_init(&w);
	(void)feed_str(&w, s);
	ntfsmac_errwatch_eof(&w);
	return ntfsmac_errwatch_clean_proof(&w, true, false, 0);
}

static uint32_t flags_of(const char *s)
{
	ntfsmac_errwatch w;

	ntfsmac_errwatch_init(&w);
	(void)feed_str(&w, s);
	return w.flags;
}

static bool is_error(const char *s)
{
	return (flags_of(s) & NTFSMAC_EW_PROTOCOL_ERROR) != 0u;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* U-DMN-EW-01: every record kind, and the clean proof of a good session. */
static void test_records(void)
{
	ntfsmac_errwatch w;

	ntfsmac_errwatch_init(&w);
	CHECK_EQ_INT(feed_str(&w, "S rw\n"), NTFSMAC_DMN_OK);
	CHECK(w.flags == NTFSMAC_EW_SAW_S_RW);
	CHECK_EQ_INT(feed_str(&w, "W flush 45\n"), NTFSMAC_DMN_OK);
	CHECK(w.flags == (NTFSMAC_EW_SAW_S_RW | NTFSMAC_EW_SAW_W));
	CHECK_EQ_INT(feed_str(&w, "U 0\n"), NTFSMAC_DMN_OK);
	CHECK((w.flags & NTFSMAC_EW_SAW_U0) != 0u);
	CHECK((w.flags & NTFSMAC_EW_SAW_U) != 0u);
	CHECK_EQ_INT(w.records, 3);
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0)); /* no EOF yet */
	ntfsmac_errwatch_eof(&w);
	CHECK(ntfsmac_errwatch_clean_proof(&w, true, false, 0)); /* a W record does not spoil it */

	CHECK(proof_of("S rw\nU 0\n"));
	CHECK(flags_of("S ro\n") == NTFSMAC_EW_SAW_S_RO);

	/* Each E operation of patch 0006. */
	CHECK(flags_of("S rw\nE pwrite 5\n") == (NTFSMAC_EW_SAW_S_RW | NTFSMAC_EW_SAW_E));
	CHECK(!is_error("S rw\nE pwrite 5\n"));
	CHECK(!is_error("S rw\nE write 28\n"));
	CHECK(!is_error("S rw\nE sync 5\n"));
	CHECK(!is_error("S rw\nE close 5\n"));
	CHECK(!is_error("S rw\nE umount 5\n"));
	CHECK(!is_error("S rw\nE pwrite 99999\n"));
	CHECK(!is_error("S rw\nW flush 25\nW flush 25\n"));

	/* U with a count: the worker tried E records, so the session is not clean. */
	CHECK((flags_of("S rw\nU 3\n") & NTFSMAC_EW_SAW_E) != 0u);
	CHECK((flags_of("S rw\nU 3\n") & NTFSMAC_EW_SAW_U0) == 0u);
	CHECK(!is_error("S rw\nU 2147483647\n"));
	CHECK(!proof_of("S rw\nU 3\n"));
	CHECK(!proof_of("S rw\nE pwrite 5\nU 1\n"));
	CHECK(!proof_of("S rw\nE pwrite 5\nU 0\n")); /* E wins over a U 0 */
	CHECK(!proof_of("S ro\nU 0\n"));             /* a ro session is not a rw proof */
}

/* U-DMN-EW-02: the grammar. Anything outside it is a protocol error. */
static void test_grammar(void)
{
	static const char *const bad[] = {
		"\n", " \n", "S\n", "S \n", "S rw \n", "S  rw\n", "S RW\n", "S rx\n", "S r\n",
		"S rww\n", "s rw\n", "X rw\n", "S\trw\n", "Srw\n",
		"S rw\nS rw\n", "S rw\nS ro\n", "S ro\nS rw\n",   /* S at most once */
		"E pwrite 5\nS rw\n", "U 0\n", "U 0\nS rw\n",       /* the first record is S */
		"W flush 5\nS rw\n",
		"S rw\nU 0\nU 0\n", "S rw\nU 0\nE pwrite 5\n",      /* nothing after U */
		"S rw\nU 0\nW flush 5\n", "S rw\nU 1\nS rw\n", "S rw\nU 2\nU 2\n",
		"S rw\nE\n", "S rw\nE \n", "S rw\nE pwrite\n", "S rw\nE pwrite \n",
		"S rw\nE pwrite 0\n", "S rw\nE pwrite 05\n", "S rw\nE pwrite -5\n",
		"S rw\nE pwrite +5\n", "S rw\nE pwrite 5x\n", "S rw\nE pwrite 100000\n",
		"S rw\nE pwrite 5 \n", "S rw\nE pwrite  5\n", "S rw\nE pwrite 5 6\n",
		"S rw\nE read 5\n", "S rw\nE flush 5\n", "S rw\nE umount\n", "S rw\nE PWRITE 5\n",
		"S rw\nE pwritee 5\n", "S rw\nE pwrit 5\n", "S rw\nE 5\n",
		"S rw\nW sync 5\n", "S rw\nW flush\n", "S rw\nW flush 0\n", "S rw\nW pwrite 5\n",
		"S rw\nU\n", "S rw\nU \n", "S rw\nU x\n", "S rw\nU -1\n", "S rw\nU 00\n", "S rw\nU 01\n",
		"S rw\nU 0 \n", "S rw\nU 2147483648\n", "S rw\nU 99999999999\n", "S rw\nU 0x1\n",
		"S rw\nU 1 2\n", "S rw\nU +1\n",
		"S rw\r\n", "S rw\nU 0\r\n", "S rw\n\n", "S rw\n\nU 0\n",
	};
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (!is_error(bad[i]))
			fprintf(stderr, "not rejected: case %zu '%s'\n", i, bad[i]);
		CHECK(is_error(bad[i]));
		CHECK(!proof_of(bad[i]));
	}
	/* A violation sets the flag of what was seen before it and nothing after. */
	CHECK((flags_of("S rw\nE pwrite 5\nS rw\nW flush 5\n") & NTFSMAC_EW_SAW_W) == 0u);
	/* After a protocol error every later byte is discarded with E_STATE, even a good U 0. */
	{
		ntfsmac_errwatch w;

		ntfsmac_errwatch_init(&w);
		CHECK_EQ_INT(feed_str(&w, "S rw\nbad\n"), NTFSMAC_DMN_E_STATE);
		CHECK_EQ_INT(feed_str(&w, "U 0\n"), NTFSMAC_DMN_E_STATE);
		CHECK((w.flags & NTFSMAC_EW_SAW_U0) == 0u);
		ntfsmac_errwatch_eof(&w);
		CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	}
}

/* U-DMN-EW-03: bounds. Long lines, forbidden bytes, record 65, endless stream. */
static void test_bounds(void)
{
	ntfsmac_errwatch w;
	char line[64], big[4096];
	size_t i, n;
	int b;

	/* A line of 31 bytes plus the newline is the longest allowed (32 bytes). */
	ntfsmac_errwatch_init(&w);
	CHECK_EQ_INT(feed_str(&w, "S rw\n"), NTFSMAC_DMN_OK);
	/* 31 bytes of content that is not in the grammar: rejected by the grammar, not the length. */
	memset(line, 'x', 31);
	line[31] = '\n';
	CHECK_EQ_INT(ntfsmac_errwatch_feed(&w, line, 32), NTFSMAC_DMN_E_STATE);
	/* 32 bytes without a newline: over the limit already. */
	ntfsmac_errwatch_init(&w);
	memset(line, 'x', 32);
	CHECK_EQ_INT(ntfsmac_errwatch_feed(&w, line, 32), NTFSMAC_DMN_E_STATE);
	/* 31 bytes without a newline is still just a partial line, and at EOF an error. */
	ntfsmac_errwatch_init(&w);
	CHECK_EQ_INT(ntfsmac_errwatch_feed(&w, line, 31), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(w.line_len, 31);
	ntfsmac_errwatch_eof(&w);
	CHECK((w.flags & NTFSMAC_EW_PROTOCOL_ERROR) != 0u);
	/* A valid record, one byte over the length with trailing spaces. */
	CHECK(is_error("S rw\nE pwrite 5                        \n"));
	CHECK(is_error("S rw                                    \n"));

	/* 100 KiB without a newline: error at byte 32, then discarded; the state stays small. */
	ntfsmac_errwatch_init(&w);
	memset(big, 'A', sizeof(big));
	for (i = 0; i < 25u; i++)
		(void)ntfsmac_errwatch_feed(&w, big, sizeof(big));
	CHECK((w.flags & NTFSMAC_EW_PROTOCOL_ERROR) != 0u);
	CHECK_EQ_INT(w.line_len, 0);
	CHECK_EQ_INT(w.records, 0);

	/* Every byte outside 0x20..0x7E inside a line is an error. */
	for (b = 0; b < 256; b++) {
		char rec[8] = "S rw\n";

		ntfsmac_errwatch_init(&w);
		(void)feed_str(&w, "S rw\nW flush 5");
		rec[0] = (char)b;
		(void)ntfsmac_errwatch_feed(&w, rec, 1);
		(void)feed_str(&w, "\n");
		if (b == '\n') /* "W flush 5" is complete; the second newline is an empty line */
			CHECK((w.flags & NTFSMAC_EW_PROTOCOL_ERROR) != 0u);
		else if (b >= 0x20 && b <= 0x7E) /* extends the number: "5x" or a digit */
			CHECK(((w.flags & NTFSMAC_EW_PROTOCOL_ERROR) != 0u) == !(b >= '0' && b <= '9'));
		else
			CHECK((w.flags & NTFSMAC_EW_PROTOCOL_ERROR) != 0u);
	}
	CHECK(is_error("S rw\nW flush 5\x7f\n"));
	CHECK(is_error("S rw\nW flush 5\x80\n"));
	CHECK(is_error("S rw\nW flush 5\xff\n"));
	{
		ntfsmac_errwatch z;

		ntfsmac_errwatch_init(&z);
		(void)ntfsmac_errwatch_feed(&z, "S rw\nW flush\0" "5\n", 16);
		CHECK((z.flags & NTFSMAC_EW_PROTOCOL_ERROR) != 0u);
	}
	CHECK(is_error("S rw\nW flush 5\t\n"));
	CHECK(is_error("\xc3\xa9\n"));

	/* 64 records are the bound: S, 62 W, U 0 is 64 and is clean; one more is an overflow. */
	{
		char *all = big; /* 4096 bytes are enough: 62 * 12 + 10 */

		n = 0;
		memcpy(all + n, "S rw\n", 5);
		n += 5;
		for (i = 0; i < 62u; i++) {
			memcpy(all + n, "W flush 25\n", 11);
			n += 11;
		}
		ntfsmac_errwatch_init(&w);
		CHECK_EQ_INT(ntfsmac_errwatch_feed(&w, all, n), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(w.records, 63);
		CHECK_EQ_INT(feed_str(&w, "U 0\n"), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(w.records, 64);
		ntfsmac_errwatch_eof(&w);
		CHECK(ntfsmac_errwatch_clean_proof(&w, true, false, 0));

		/* 65 records: S, 63 W, U 0. */
		ntfsmac_errwatch_init(&w);
		memcpy(all + n, "W flush 25\n", 11);
		n += 11;
		CHECK_EQ_INT(ntfsmac_errwatch_feed(&w, all, n), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(w.records, 64);
		CHECK_EQ_INT(feed_str(&w, "U 0\n"), NTFSMAC_DMN_E_STATE); /* record 65 */
		CHECK((w.flags & NTFSMAC_EW_PROTOCOL_ERROR) != 0u);
		CHECK((w.flags & NTFSMAC_EW_SAW_U0) == 0u);
		ntfsmac_errwatch_eof(&w);
		CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));
		/* 65 records that all are good but the last: the 65th W is also refused. */
		ntfsmac_errwatch_init(&w);
		CHECK_EQ_INT(ntfsmac_errwatch_feed(&w, all, n), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(feed_str(&w, "W flush 25\n"), NTFSMAC_DMN_E_STATE);
		CHECK_EQ_INT(w.records, 65);
	}

	/* Endless stream of valid records: bounded by the record count, memory is the struct. */
	ntfsmac_errwatch_init(&w);
	(void)feed_str(&w, "S rw\n");
	for (i = 0; i < 100000u; i++)
		(void)feed_str(&w, "W flush 25\n");
	CHECK((w.flags & NTFSMAC_EW_PROTOCOL_ERROR) != 0u);
	CHECK_EQ_INT(w.records, 65); /* counting stops at the overflow */
	/* Endless stream of garbage in 4 KiB pieces. */
	ntfsmac_errwatch_init(&w);
	memset(big, 'z', sizeof(big));
	for (i = 0; i < 4000u; i++)
		(void)ntfsmac_errwatch_feed(&w, big, sizeof(big));
	CHECK((w.flags & NTFSMAC_EW_PROTOCOL_ERROR) != 0u);
	CHECK_EQ_INT(w.line_len, 0);
}

/* U-DMN-EW-04: the result does not depend on how the bytes are cut into reads. */
static void test_chunking(void)
{
	static const char *const cases[] = {
		"S rw\nW flush 25\nU 0\n",
		"S rw\nE pwrite 5\nU 1\n",
		"S rw\nE pwrite 5\nS rw\nU 0\n",
		"S rw\nW flush 25\nU 0\nX\n",
		"S rw\nU 0",
		"S rw\n\nU 0\n",
	};
	size_t c;

	for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		const char *s = cases[c];
		size_t len = strlen(s), cut, i;
		ntfsmac_errwatch whole;

		ntfsmac_errwatch_init(&whole);
		(void)ntfsmac_errwatch_feed(&whole, s, len);
		ntfsmac_errwatch_eof(&whole);
		for (cut = 0; cut <= len; cut++) {
			ntfsmac_errwatch w;

			ntfsmac_errwatch_init(&w);
			(void)ntfsmac_errwatch_feed(&w, s, cut);
			(void)ntfsmac_errwatch_feed(&w, s + cut, len - cut);
			ntfsmac_errwatch_eof(&w);
			CHECK(w.flags == whole.flags);
			CHECK_EQ_INT(w.records, whole.records);
		}
		{
			ntfsmac_errwatch w; /* one byte at a time */

			ntfsmac_errwatch_init(&w);
			for (i = 0; i < len; i++)
				(void)ntfsmac_errwatch_feed(&w, s + i, 1);
			ntfsmac_errwatch_eof(&w);
			CHECK(w.flags == whole.flags);
		}
	}
	{
		ntfsmac_errwatch w;

		ntfsmac_errwatch_init(&w);
		CHECK_EQ_INT(ntfsmac_errwatch_feed(&w, NULL, 0), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(ntfsmac_errwatch_feed(&w, "", 0), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(w.flags, 0);
	}
}

/* U-DMN-EW-05: the proof needs every part; each missing part gives "not clean". */
static void test_proof(void)
{
	ntfsmac_errwatch w;
	const char *good = "S rw\nU 0\n";

	ntfsmac_errwatch_init(&w);
	(void)feed_str(&w, good);
	ntfsmac_errwatch_eof(&w);
	CHECK(ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	CHECK(!ntfsmac_errwatch_clean_proof(&w, false, false, 0)); /* unmount not seen first */
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, true, 0));   /* re-adopted */
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 1));  /* non-zero exit */
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, -1)); /* killed by a signal */
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 256));
	CHECK(!ntfsmac_errwatch_clean_proof(NULL, true, false, 0));

	/* Silence, a missing S or U, a partial line, no EOF, data after EOF. */
	CHECK(!proof_of(""));
	CHECK(!proof_of("S rw\n"));
	CHECK(!proof_of("U 0\n"));
	CHECK(!proof_of("S rw\nW flush 5\n"));
	CHECK(!proof_of("S rw\nU 0"));          /* no newline at the end */
	CHECK(!proof_of("S rw\nU 0\nU 0"));
	ntfsmac_errwatch_init(&w);
	(void)feed_str(&w, good); /* EOF never came */
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	ntfsmac_errwatch_eof(&w);
	CHECK_EQ_INT(feed_str(&w, "x"), NTFSMAC_DMN_E_STATE); /* byte after EOF */
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	ntfsmac_errwatch_init(&w);
	(void)feed_str(&w, good);
	ntfsmac_errwatch_eof(&w);
	CHECK_EQ_INT(feed_str(&w, ""), NTFSMAC_DMN_OK); /* an empty read after EOF is harmless */
	CHECK(ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	ntfsmac_errwatch_eof(&w); /* twice */
	CHECK(ntfsmac_errwatch_clean_proof(&w, true, false, 0));

	/* An empty watcher is never clean, also at EOF; NULL arguments are safe. */
	ntfsmac_errwatch_init(&w);
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	ntfsmac_errwatch_eof(&w);
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	ntfsmac_errwatch_init(NULL);
	ntfsmac_errwatch_eof(NULL);
	CHECK_EQ_INT(ntfsmac_errwatch_feed(NULL, "x", 1), NTFSMAC_DMN_E_INVAL);
	ntfsmac_errwatch_init(&w);
	CHECK_EQ_INT(ntfsmac_errwatch_feed(&w, NULL, 1), NTFSMAC_DMN_E_INVAL);

	/* Handcrafted flags that the parser never produces still do not prove anything. */
	ntfsmac_errwatch_init(&w);
	w.flags = NTFSMAC_EW_SAW_S_RW | NTFSMAC_EW_SAW_U0 | NTFSMAC_EW_EOF; /* no SAW_U */
	w.records = 2;
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	w.flags |= NTFSMAC_EW_SAW_U;
	CHECK(ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	w.records = 0;
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	w.records = 65;
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	w.records = 2;
	w.line_len = 1;
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));
}

/* U-DMN-EW-06: pipe hygiene. */
static void test_pipe(void)
{
	int rd = -1, wr = -1, fl;
	int rd2 = -1;

	CHECK_EQ_INT(ntfsmac_errwatch_pipe_open(NULL, &wr), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_errwatch_pipe_open(&rd, NULL), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_errwatch_pipe_open(&rd, &wr), NTFSMAC_DMN_OK);
	CHECK(rd >= 3 && wr >= 3 && rd != wr);
	fl = fcntl(rd, F_GETFD);
	CHECK(fl != -1 && (fl & FD_CLOEXEC) != 0);
	fl = fcntl(wr, F_GETFD);
	CHECK(fl != -1 && (fl & FD_CLOEXEC) != 0);
	fl = fcntl(rd, F_GETFL);
	CHECK(fl != -1 && (fl & O_NONBLOCK) != 0);
	fl = fcntl(wr, F_GETFL);
	CHECK(fl != -1 && (fl & O_NONBLOCK) == 0); /* the worker's end is not changed by the daemon */
	CHECK((fcntl(rd, F_GETFL) & O_ACCMODE) == O_RDONLY);
	CHECK((fcntl(wr, F_GETFL) & O_ACCMODE) == O_WRONLY);
	close(rd);
	close(wr);

	/* With descriptors 0..2 closed the ends are still 3 or higher. */
	{
		int save[3], i;

		for (i = 0; i < 3; i++)
			save[i] = dup(i);
		for (i = 0; i < 3; i++)
			close(i);
		CHECK_EQ_INT(ntfsmac_errwatch_pipe_open(&rd2, &wr), NTFSMAC_DMN_OK);
		CHECK(rd2 >= 3 && wr >= 3);
		close(rd2);
		close(wr);
		for (i = 0; i < 3; i++) {
			dup2(save[i], i);
			close(save[i]);
		}
	}
}

/* U-DMN-EW-07: reading from a real pipe; EOF timing; the endless writer. */
static void test_reading(void)
{
	int rd, wr;
	ntfsmac_errwatch w;
	uint64_t t0, dt;
	pid_t pid;
	int st;

	/* A whole clean session, then EOF: drain returns OK at once. */
	CHECK_EQ_INT(ntfsmac_errwatch_pipe_open(&rd, &wr), NTFSMAC_DMN_OK);
	CHECK_EQ_INT((int)write(wr, "S rw\nW flush 25\nU 0\n", 20), 20);
	close(wr);
	ntfsmac_errwatch_init(&w);
	t0 = now_ms();
	CHECK_EQ_INT(ntfsmac_errwatch_drain(&w, rd, NTFSMAC_ERRWATCH_EOF_WAIT_MS), NTFSMAC_DMN_OK);
	CHECK(now_ms() - t0 < 1000u);
	CHECK(ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	CHECK_EQ_INT(ntfsmac_errwatch_pump(&w, rd), NTFSMAC_DMN_OK); /* again at EOF */
	close(rd);

	/* The write end is still held (a leaked descriptor): no EOF, drain stops at its deadline. */
	CHECK_EQ_INT(ntfsmac_errwatch_pipe_open(&rd, &wr), NTFSMAC_DMN_OK);
	CHECK_EQ_INT((int)write(wr, "S rw\nU 0\n", 9), 9);
	ntfsmac_errwatch_init(&w);
	CHECK_EQ_INT(ntfsmac_errwatch_pump(&w, rd), NTFSMAC_DMN_OK); /* does not block */
	CHECK((w.flags & NTFSMAC_EW_EOF) == 0u);
	t0 = now_ms();
	CHECK_EQ_INT(ntfsmac_errwatch_drain(&w, rd, 300), NTFSMAC_DMN_E_TIMEOUT);
	dt = now_ms() - t0;
	CHECK(dt >= 290u && dt < 1500u);
	CHECK((w.flags & NTFSMAC_EW_EOF) == 0u);
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	/* EOF that comes later than the deadline is not retroactively accepted by drain, but a
	 * later pump sees it. */
	close(wr);
	CHECK_EQ_INT(ntfsmac_errwatch_pump(&w, rd), NTFSMAC_DMN_OK);
	CHECK((w.flags & NTFSMAC_EW_EOF) != 0u);
	close(rd);

	/* The write end is closed late, inside the wait: EOF is seen. */
	CHECK_EQ_INT(ntfsmac_errwatch_pipe_open(&rd, &wr), NTFSMAC_DMN_OK);
	pid = fork();
	if (pid == 0) {
		close(rd);
		usleep(200000);
		(void)!write(wr, "S rw\nU 0\n", 9);
		_exit(0);
	}
	close(wr);
	ntfsmac_errwatch_init(&w);
	CHECK_EQ_INT(ntfsmac_errwatch_drain(&w, rd, NTFSMAC_ERRWATCH_EOF_WAIT_MS), NTFSMAC_DMN_OK);
	CHECK(ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	waitpid(pid, &st, 0);
	close(rd);

	/* An endless writer: drain still ends at its deadline, state stays bounded. */
	CHECK_EQ_INT(ntfsmac_errwatch_pipe_open(&rd, &wr), NTFSMAC_DMN_OK);
	pid = fork();
	if (pid == 0) {
		char junk[4096];

		close(rd);
		memset(junk, 'q', sizeof(junk));
		for (;;)
			(void)!write(wr, junk, sizeof(junk));
	}
	close(wr);
	ntfsmac_errwatch_init(&w);
	t0 = now_ms();
	CHECK_EQ_INT(ntfsmac_errwatch_drain(&w, rd, 300), NTFSMAC_DMN_E_TIMEOUT);
	dt = now_ms() - t0;
	CHECK(dt >= 290u && dt < 2000u);
	CHECK((w.flags & NTFSMAC_EW_PROTOCOL_ERROR) != 0u);
	CHECK(w.line_len == 0u);
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	/* The writer is gone: the rest is read to EOF. */
	CHECK_EQ_INT(ntfsmac_errwatch_drain(&w, rd, NTFSMAC_ERRWATCH_EOF_WAIT_MS), NTFSMAC_DMN_OK);
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));
	close(rd);

	/* Bad arguments and a closed descriptor. */
	CHECK_EQ_INT(ntfsmac_errwatch_pump(NULL, 3), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_errwatch_pump(&w, -1), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_errwatch_drain(NULL, 3, 10), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_errwatch_drain(&w, -1, 10), NTFSMAC_DMN_E_INVAL);
	ntfsmac_errwatch_init(&w);
	CHECK_EQ_INT(ntfsmac_errwatch_pump(&w, rd), NTFSMAC_DMN_E_IO); /* rd is closed */
	CHECK((w.flags & NTFSMAC_EW_EOF) == 0u);
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	test_records();
	test_grammar();
	test_bounds();
	test_chunking();
	test_proof();
	test_pipe();
	test_reading();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
