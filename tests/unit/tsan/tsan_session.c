/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-TSAN-03 (T-M4-14): the session module and the privileged operations on shared state.
 *
 * NSES "volume queue" threads, one per serial, run whole sessions with ntfsmac_session_begin(),
 * _note_write_error() and _end() on one shared journal, with a fake system (nothing shared but
 * the journal). While they run, NPRIV threads send journal_clear and repair requests for the
 * same serials through ntfsmac_priv_journal_clear() and ntfsmac_priv_repair(), and one
 * thread runs ntfsmac_session_decide() the way an attach would. The registry fact "a live
 * session owns this entry" is an atomic per serial, set before begin() and cleared after end(),
 * as the daemon's registry marks a volume Mounting before the session starts.
 *
 * Invariants: a session that was live is never robbed of its entry (a clean end always removes
 * it); every reply is one line; the journal is whole at the end. Run with the `tsan` preset for
 * the race check; it also runs as a plain concurrency test in the other builds.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "internal.h"
#include "ntfsmac/ctl_proto.h"

#define NSES 4
#define NPRIV 2
#define ROUNDS 150
#define BASE UINT64_C(0xAB00000000000000)

static ntfsmac_journal *g_j;
static atomic_int g_live[NSES]; /* the registry: a session of serial BASE+i is Mounting or Mounted */
static atomic_int g_cleared_live;
static atomic_int g_priv_ok, g_priv_refused;
static atomic_int g_stop;

/* ---------------- fake session system (per thread) ---------------- */

typedef struct tctx {
	unsigned idx;
	int open_fds;
	uint64_t seq;
	uint64_t clock;
} tctx;

static int s_open(void *c, const char *bsd, bool w)
{
	tctx *t = c;

	(void)bsd;
	(void)w;
	t->open_fds++;
	return 10 + t->open_fds;
}

static void s_close(void *c, int fd)
{
	tctx *t = c;

	(void)fd;
	t->open_fds--;
}

static int s_id(void *c, int fd, ntfsmac_dev_id *out)
{
	(void)c;
	(void)fd;
	out->a = 1;
	out->b = 1;
	return 0;
}

static int s_block(void *c, int fd, uint8_t *buf)
{
	(void)c;
	(void)fd;
	memset(buf, 0x5A, NTFSMAC_BLOCK0_BYTES);
	return 0;
}

static int s_flags(void *c, int fd)
{
	(void)c;
	(void)fd;
	return 0;
}

static ntfsmac_dmn_status s_describe(void *c, const char *bsd, ntfsmac_disk_info *out)
{
	(void)c;
	memset(out, 0, sizeof *out);
	snprintf(out->bsd_name, sizeof out->bsd_name, "%s", bsd);
	snprintf(out->media_path, sizeof out->media_path, "P");
	return NTFSMAC_DMN_OK;
}

static ntfsmac_dmn_status s_volmark(void *c, const ntfsmac_volmark_run *r, ntfsmac_volmark_op op,
				    int *exit_code)
{
	(void)c;
	(void)r;
	(void)op;
	*exit_code = 0;
	return NTFSMAC_DMN_OK;
}

static ntfsmac_dmn_status s_sync(void *c, int fd, bool *flush_ok)
{
	(void)c;
	(void)fd;
	*flush_ok = true;
	return NTFSMAC_DMN_OK;
}

/*
 * One second per call: a tombstone is named by the second (section 3.5), so two removals of the
 * same serial inside one second would collide and the second entry would stay. The fake clock
 * never repeats a second for one thread.
 */
static int64_t s_now(void *c)
{
	tctx *t = c;

	return (int64_t)1700000000 + (int64_t)t->idx * 100000 + (int64_t)++t->clock;
}

/* The session id says whose it is: (index + 1) in the high half. */
static uint64_t s_sid(void *c)
{
	tctx *t = c;

	return ((uint64_t)(t->idx + 1) << 32) | ++t->seq;
}

static ntfsmac_probe_verdict verdict_for(unsigned i)
{
	ntfsmac_probe_verdict v;

	memset(&v, 0, sizeof v);
	v.is_ntfs = true;
	v.has_ntfs_version = true;
	v.ntfs_version_major = 3;
	v.ntfs_version_minor = 1;
	v.serial = BASE + i;
	v.total_sectors = 4096;
	v.cluster_size = 4096;
	v.sector_size = 512;
	v.logfile = NTFSMAC_PROBE_LOGFILE_CLEAN;
	v.has_logfile_lsn = true;
	v.logfile_lsn = 100;
	v.hiberfile = NTFSMAC_PROBE_HIBERFILE_ABSENT;
	v.state = NTFSMAC_PROBE_STATE_CLEAN;
	snprintf(v.label, sizeof v.label, "V%u", i);
	v.label_len = strlen(v.label);
	return v;
}

static void *session_main(void *arg)
{
	tctx t;
	ntfsmac_session_sys sys;
	ntfsmac_session_config cfg;
	unsigned r;

	memset(&t, 0, sizeof t);
	t.idx = (unsigned)(uintptr_t)arg;
	memset(&sys, 0, sizeof sys);
	sys.ctx = &t;
	sys.open_device = s_open;
	sys.close_fd = s_close;
	sys.dev_identity = s_id;
	sys.read_block0 = s_block;
	sys.reset_flags = s_flags;
	sys.describe = s_describe;
	sys.volmark = s_volmark;
	sys.sync_device = s_sync;
	sys.now = s_now;
	sys.new_session_id = s_sid;
	memset(&cfg, 0, sizeof cfg);

	for (r = 0; r < ROUNDS; r++) {
		ntfsmac_probe_verdict v = verdict_for(t.idx);
		ntfsmac_session_decision d;
		ntfsmac_session_begin_in in;
		ntfsmac_session *s = NULL;
		ntfsmac_session_start res;
		ntfsmac_session_end_result er;
		ntfsmac_errwatch w;
		ntfsmac_dmn_status st;
		static const char *const good = "S rw\nU 0\n";
		static const char *const bad = "S rw\nE write 5\nU 1\n";
		bool want_error = (r % 3) == 1;

		/* the registry marks the volume before the session starts */
		atomic_store(&g_live[t.idx], 1);
		CHECK(ntfsmac_session_decide(&cfg, g_j, &v, true, false, s_now(&t), NULL, NULL, &d) ==
		      NTFSMAC_DMN_OK);
		if (d.policy.decision != NTFSMAC_POLICY_RW) {
			/* an entry is there (an unclean end of an earlier round, not yet cleared) */
			atomic_store(&g_live[t.idx], 0);
			usleep(200);
			continue;
		}
		memset(&in, 0, sizeof in);
		in.bsd = "disk4s1";
		in.verdict = &v;
		in.decision = NTFSMAC_POLICY_RW;
		in.probe_fd = s_open(&t, "disk4s1", false);
		memset(in.block0, 0x5A, sizeof in.block0);
		snprintf(in.media_path, sizeof in.media_path, "P");
		st = ntfsmac_session_begin(&sys, &cfg, g_j, &in, &s, &res);
		CHECK(st == NTFSMAC_DMN_OK && s != NULL && res.mode == NTFSMAC_VMODE_RW);
		if (s == NULL) {
			atomic_store(&g_live[t.idx], 0);
			continue;
		}
		if (want_error)
			CHECK(ntfsmac_session_note_write_error(s) == NTFSMAC_DMN_OK);
		usleep((useconds_t)(50 + (r % 7) * 30)); /* a session lasts a little */
		ntfsmac_errwatch_init(&w);
		ntfsmac_errwatch_feed(&w, want_error ? bad : good, strlen(want_error ? bad : good));
		ntfsmac_errwatch_eof(&w);
		CHECK(ntfsmac_session_end(s, &w, true, 0, false, &er) == NTFSMAC_DMN_OK);
		/*
		 * A live session is never robbed of its entry: if the proof held, the removal found
		 * the entry and the report is CLEAN. An unclean one leaves it for journal-clear.
		 */
		if (!want_error) {
			CHECK(er.report == NTFSMAC_SREPORT_CLEAN);
			if (er.report != NTFSMAC_SREPORT_CLEAN)
				atomic_fetch_add(&g_cleared_live, 1);
		} else {
			CHECK(er.report == NTFSMAC_SREPORT_UNCLEAN);
		}
		ntfsmac_session_free(s);
		CHECK(t.open_fds == 0);
		atomic_store(&g_live[t.idx], 0);
	}
	return NULL;
}

/* ---------------- fake privileged system ---------------- */

static bool p_volume(void *c, const char *bsd, ntfsmac_priv_volume *out)
{
	(void)c;
	(void)bsd;
	out->mounted = false;
	out->mounted_rw = false;
	return true;
}

static __thread unsigned t_serial_idx; /* the serial this thread's request is about */

static ntfsmac_dmn_status p_probe(void *c, const char *bsd, ntfsmac_probe_verdict *out,
				  ntfsmac_priv_bind *bind)
{
	(void)c;
	(void)bsd;
	bind->fd = -1;
	*out = verdict_for(t_serial_idx);
	return NTFSMAC_DMN_OK;
}

static bool p_live(void *c, uint64_t id)
{
	(void)c;
	if (id == 0)
		return false;
	return atomic_load(&g_live[(id >> 32) - 1]) != 0;
}

static ntfsmac_dmn_status p_clear(void *c, const char *bsd, const ntfsmac_probe_verdict *v,
				  ntfsmac_priv_bind *bind, int *ec)
{
	(void)c;
	(void)bsd;
	(void)v;
	(void)bind;
	*ec = 0;
	return NTFSMAC_DMN_OK;
}

static int64_t p_now(void *c)
{
	(void)c;
	return (int64_t)time(NULL);
}

static void *priv_main(void *arg)
{
	ntfsmac_priv_sys ps;
	ntfsmac_priv priv;
	ntfsmac_ctl_peer peer;
	unsigned who = (unsigned)(uintptr_t)arg, r;
	uint64_t rs = 0x9E3779B97F4A7C15ull * (who + 1);

	memset(&ps, 0, sizeof ps);
	ps.volume = p_volume;
	ps.probe_fresh = p_probe;
	ps.session_live = p_live;
	ps.clear_dirty = p_clear;
	ps.now = p_now;
	priv.sys = &ps;
	priv.journal = g_j;
	memset(&peer, 0, sizeof peer);
	peer.admin = true;

	for (r = 0; !atomic_load(&g_stop); r++) {
		char reply[NTFSMAC_CTL_REPLY_MIN * 2];
		char serial[16], label[16];
		ntfsmac_dmn_status st;

		rs = rs * 6364136223846793005ull + 1442695040888963407ull;
		t_serial_idx = (unsigned)((rs >> 33) % NSES);
		snprintf(label, sizeof label, "V%u", t_serial_idx);
		snprintf(serial, sizeof serial, "%08X", (unsigned)((BASE + t_serial_idx) & 0xFFFFFFFFu));
		if ((rs >> 20) % 3 == 0) {
			/* repair on a clean volume: refused by the gate, never writes */
			st = ntfsmac_priv_repair(&priv, &peer, "disk4s1", "clear_dirty", label, serial, reply,
						 sizeof reply);
			CHECK(st == NTFSMAC_DMN_E_DENIED);
		} else {
			st = ntfsmac_priv_journal_clear(&priv, &peer, "disk4s1", label, serial, reply,
							sizeof reply);
			if (getenv("TSAN_SES_DEBUG") != NULL && r < 400 && (r % 100) == 0)
				fprintf(stderr, "reply: %s\n", reply);
			if (st == NTFSMAC_DMN_OK)
				atomic_fetch_add(&g_priv_ok, 1);
			else
				atomic_fetch_add(&g_priv_refused, 1);
			CHECK(st == NTFSMAC_DMN_OK || st == NTFSMAC_DMN_E_DENIED || st == NTFSMAC_DMN_E_IO);
		}
		CHECK(strncmp(reply, "{\"ok\":", 6) == 0 && strchr(reply, '\n') == NULL);
		usleep(100);
	}
	return NULL;
}

static int list_cb(const ntfsmac_journal_entry *e, void *ctx)
{
	(void)ctx;
	CHECK(e->serial >= BASE && e->serial < BASE + NSES);
	return 0;
}

static void *reader_main(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_stop)) {
		unsigned skipped = 0;

		CHECK(ntfsmac_journal_list(g_j, list_cb, NULL, &skipped) == NTFSMAC_JOURNAL_OK);
		CHECK(skipped == 0);
		usleep(300);
	}
	return NULL;
}

int main(void)
{
	const char *t = getenv("TMPDIR");
	char dir[160], jdir[200], cmd[400];
	pthread_t ses[NSES], pv[NPRIV], rd;
	unsigned i;

	snprintf(dir, sizeof dir, "%s%sntfsmac-tsan-ses-XXXXXX", t != NULL ? t : "/tmp",
		 (t != NULL && t[strlen(t) - 1] == '/') ? "" : "/");
	CHECK(mkdtemp(dir) != NULL);
	snprintf(jdir, sizeof jdir, "%s/j", dir);
	CHECK(ntfsmac_journal_open(jdir, &g_j) == NTFSMAC_JOURNAL_OK);

	CHECK(pthread_create(&rd, NULL, reader_main, NULL) == 0);
	for (i = 0; i < NSES; i++)
		CHECK(pthread_create(&ses[i], NULL, session_main, (void *)(uintptr_t)i) == 0);
	for (i = 0; i < NPRIV; i++)
		CHECK(pthread_create(&pv[i], NULL, priv_main, (void *)(uintptr_t)i) == 0);
	for (i = 0; i < NSES; i++)
		pthread_join(ses[i], NULL);
	atomic_store(&g_stop, 1); /* the privileged requests and the reader run while sessions do */
	for (i = 0; i < NPRIV; i++)
		pthread_join(pv[i], NULL);
	pthread_join(rd, NULL);

	CHECK(atomic_load(&g_cleared_live) == 0);
	{
		unsigned skipped = 0;

		CHECK(ntfsmac_journal_list(g_j, list_cb, NULL, &skipped) == NTFSMAC_JOURNAL_OK);
		CHECK(skipped == 0);
	}
	ntfsmac_journal_close(g_j);
	printf("tsan_session: journal-clear reached %d, refused %d, live sessions robbed %d\n",
	       atomic_load(&g_priv_ok), atomic_load(&g_priv_refused), atomic_load(&g_cleared_live));
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); /* our own temporary directory */
	if (system(cmd) != 0)
		fprintf(stderr, "cleanup failed\n");
	return check_failures() == 0 ? 0 : 1;
}
