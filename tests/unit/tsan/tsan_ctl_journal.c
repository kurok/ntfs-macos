/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-TSAN-02: the control-protocol handler with concurrent clients, while the
 * journal is updated concurrently, and a kill at random points.
 *
 * Phase 1, kill: a child process creates, updates and removes one journal
 * entry in a loop and is killed with SIGKILL at a random moment while reader
 * threads in the parent read the entry and list the directory. A read never
 * sees a partial entry: `get` gives OK with a whole entry or NOT_FOUND, never
 * CORRUPT, and `list` skips nothing.
 *
 * Phase 2, concurrent clients and journal updates:
 *   - NVOL "volume queue" threads, one per serial (the model of section 5 of
 *     ARCHITECTURE.md), run the journal side of a session: create, marker
 *     pending to set, state write_error, clean removal. They share one
 *     journal handle, as the daemon does.
 *   - The control socket server runs on its own thread (ctl.c). NCLIENT
 *     client threads talk to it over the socket (pipelined requests, several
 *     connections each) with read commands, mount, unmount and privileged
 *     requests that carry fresh authorization forms.
 *   - NDIRECT threads call ntfsmac_ctl_handle_line_env() themselves with the
 *     same environment (one shared replay table), the way a second server
 *     thread would.
 *   - The operations behind the handler read and write the same journal as
 *     the volume threads: `list` and `status` read it, `journal_clear`
 *     removes entries.
 *   Replay: the same authorization form sent by all direct threads at the same
 *   moment must reach the privileged operation exactly once.
 *
 * Session parts (ntfsmac_session_begin and _end) are deferred: src/daemon/
 * session.c is a stub until T-M4-14. When it lands, the volume threads must
 * call it in place of the journal calls below, and a kill phase for the
 * session channel can be added here.
 *
 * Run it with the `tsan` preset (cmake --preset tsan) to get the race check;
 * it also runs in the normal build as a plain concurrency test.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "ctl_priv.h"
#include "ntfsmac/journal.h"
#include "ntfsmac/log.h"

#define NVOL 4
#define NCLIENT 4
#define NDIRECT 3
#define VOL_ROUNDS 12
#define CLIENT_CONNS 20
#define DIRECT_LINES 150
#define SHARED_FORMS 20
#define KILL_ROUNDS 6
#define VOL_SERIAL(i) (0x100u + (unsigned)(i))
#define SHARED_SERIAL(k) (0x1000u + (unsigned)(k))
#define KILL_SERIAL 0x77u

static char g_dir[160];
static char g_jdir[192];
static char g_sock[192];
static ntfsmac_journal *g_j;

/* ---------------- helpers ---------------- */

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms)
{
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

	nanosleep(&ts, NULL);
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

/*
 * Each removal gets its own time, so two removals of one serial never share a
 * tombstone name (the journal refuses a second removal in the same second).
 */
static pthread_mutex_t g_time_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_time = 1700000000;

static int64_t next_time(void)
{
	int64_t t;

	pthread_mutex_lock(&g_time_lock);
	t = ++g_time;
	pthread_mutex_unlock(&g_time_lock);
	return t;
}

static ntfsmac_journal_entry new_entry(uint64_t serial)
{
	ntfsmac_journal_entry e;

	memset(&e, 0, sizeof e);
	e.serial = serial;
	snprintf(e.label, sizeof e.label, "TSAN%llu", (unsigned long long)serial);
	e.total_sectors = 976773167u;
	e.session_id = 0x1122334455667788ull;
	e.created = 1700000000;
	e.updated = 1700000000;
	e.logfile_lsn_at_start = 0x1ABCDEF01ull;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	e.state = NTFSMAC_JOURNAL_STATE_OPEN;
	return e;
}

/* An entry that the journal returned must be whole and say what it was created with. */
static void check_entry_whole(const ntfsmac_journal_entry *e)
{
	char want[NTFSMAC_JOURNAL_LABEL_MAX_BYTES + 1];

	snprintf(want, sizeof want, "TSAN%llu", (unsigned long long)e->serial);
	CHECK(strcmp(e->label, want) == 0);
	CHECK(e->total_sectors == 976773167u);
	CHECK(e->session_id == 0x1122334455667788ull);
	CHECK(e->created == 1700000000);
	CHECK(e->updated >= e->created);
	CHECK(e->logfile_lsn_at_start == 0x1ABCDEF01ull);
	CHECK(e->marker >= NTFSMAC_JOURNAL_MARKER_PENDING && e->marker <= NTFSMAC_JOURNAL_MARKER_NONE);
	CHECK(e->state == NTFSMAC_JOURNAL_STATE_OPEN || e->state == NTFSMAC_JOURNAL_STATE_WRITE_ERROR);
}

static int list_check_cb(const ntfsmac_journal_entry *e, void *ctx)
{
	(void)ctx;
	check_entry_whole(e);
	return 0;
}

/* ---------------- phase 1: kill at random points ---------------- */

static atomic_int g_stop_readers;

static void *kill_reader_main(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_stop_readers)) {
		ntfsmac_journal_entry e;
		unsigned skipped = 0;
		ntfsmac_journal_status st = ntfsmac_journal_get(g_j, KILL_SERIAL, 976773167u, &e);

		CHECK(st == NTFSMAC_JOURNAL_OK || st == NTFSMAC_JOURNAL_ERR_NOT_FOUND);
		if (st == NTFSMAC_JOURNAL_OK)
			check_entry_whole(&e);
		st = ntfsmac_journal_list(g_j, list_check_cb, NULL, &skipped);
		CHECK(st == NTFSMAC_JOURNAL_OK);
		CHECK(skipped == 0);
	}
	return NULL;
}

/* The child: one thread only (fork happens before any thread of this test exists). */
static void kill_child(int round)
{
	ntfsmac_journal *j;
	int64_t t = 1800000000 + (int64_t)round * 1000000; /* tombstone names never repeat */

	if (ntfsmac_journal_open(g_jdir, &j) != NTFSMAC_JOURNAL_OK)
		_exit(3);
	for (;;) {
		ntfsmac_journal_entry e = new_entry(KILL_SERIAL);

		if (ntfsmac_journal_create(j, &e) != NTFSMAC_JOURNAL_OK)
			_exit(4);
		e.marker = NTFSMAC_JOURNAL_MARKER_SET;
		e.updated++;
		if (ntfsmac_journal_update(j, &e) != NTFSMAC_JOURNAL_OK)
			_exit(5);
		e.state = NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
		e.updated++;
		if (ntfsmac_journal_update(j, &e) != NTFSMAC_JOURNAL_OK)
			_exit(6);
		if (ntfsmac_journal_remove(j, KILL_SERIAL, 976773167u, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR,
					   ++t) != NTFSMAC_JOURNAL_OK)
			_exit(7);
	}
}

static void phase_kill(void)
{
	uint64_t rs = 0xC0FFEE1234567ull;
	int round;

	for (round = 0; round < KILL_ROUNDS; round++) {
		pthread_t rd[2];
		pid_t pid;
		int status, k;

		atomic_store(&g_stop_readers, 0);
		pid = fork(); /* no other thread exists yet */
		if (pid < 0) {
			CHECK(false);
			return;
		}
		if (pid == 0)
			kill_child(round); /* never returns */
		for (k = 0; k < 2; k++)
			CHECK(pthread_create(&rd[k], NULL, kill_reader_main, NULL) == 0);
		sleep_ms(3 + (long)(rnd_next(&rs) % 60u)); /* a random point in the loop */
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
		if (WIFEXITED(status))
			fprintf(stderr, "kill child exited with %d\n", WEXITSTATUS(status));
		sleep_ms(5); /* readers see the state the kill left */
		atomic_store(&g_stop_readers, 1);
		for (k = 0; k < 2; k++)
			pthread_join(rd[k], NULL);

		/* After the kill, the entry is either whole or absent. */
		{
			ntfsmac_journal_entry e;
			ntfsmac_journal_status st = ntfsmac_journal_get(g_j, KILL_SERIAL, 976773167u, &e);

			CHECK(st == NTFSMAC_JOURNAL_OK || st == NTFSMAC_JOURNAL_ERR_NOT_FOUND);
			if (st == NTFSMAC_JOURNAL_OK) {
				check_entry_whole(&e);
				/* the next child must start clean */
				CHECK(ntfsmac_journal_remove(g_j, KILL_SERIAL, 976773167u,
							     NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR,
							     next_time()) == NTFSMAC_JOURNAL_OK);
			}
		}
	}
}

/* ---------------- operations behind the handler ---------------- */

typedef struct ops_state {
	pthread_mutex_t mu;
	int calls[NTFSMAC_CTL_CMD_COUNT_];
	int shared_reached[SHARED_FORMS];
	int list_entries_seen;
} ops_state;

static ops_state g_ops;

static void ops_count(ntfsmac_ctl_cmd c)
{
	pthread_mutex_lock(&g_ops.mu);
	g_ops.calls[c]++;
	pthread_mutex_unlock(&g_ops.mu);
}

struct list_acc {
	char *buf;
	size_t cap, len;
	int n;
};

static int list_acc_cb(const ntfsmac_journal_entry *e, void *ctx)
{
	struct list_acc *a = ctx;
	int w;

	check_entry_whole(e);
	w = snprintf(a->buf + a->len, a->cap - a->len, "%s{\"serial\":%llu,\"state\":%d}",
		     a->n > 0 ? "," : "", (unsigned long long)e->serial, (int)e->state);
	if (w > 0 && (size_t)w < a->cap - a->len)
		a->len += (size_t)w;
	a->n++;
	return 0;
}

static ntfsmac_dmn_status op_list(void *ctx, char *reply, size_t cap)
{
	char body[1024];
	struct list_acc a = {body, sizeof body, 0, 0};
	unsigned skipped = 0;

	(void)ctx;
	ops_count(NTFSMAC_CTL_CMD_LIST);
	body[0] = '\0';
	CHECK(ntfsmac_journal_list(g_j, list_acc_cb, &a, &skipped) == NTFSMAC_JOURNAL_OK);
	CHECK(skipped == 0); /* never a partial entry */
	pthread_mutex_lock(&g_ops.mu);
	g_ops.list_entries_seen += a.n;
	pthread_mutex_unlock(&g_ops.mu);
	snprintf(reply, cap, "{\"ok\":true,\"volumes\":[%s]}", body);
	return NTFSMAC_DMN_OK;
}

static ntfsmac_dmn_status op_status(void *ctx, const char *bsd, char *reply, size_t cap)
{
	ntfsmac_journal_entry e;
	ntfsmac_journal_status st;
	unsigned n = 0;

	(void)ctx;
	ops_count(NTFSMAC_CTL_CMD_STATUS);
	if (sscanf(bsd, "disk%us1", &n) != 1 || n >= NVOL) {
		snprintf(reply, cap, "{\"ok\":false,\"error\":\"no_such_volume\"}");
		return NTFSMAC_DMN_OK;
	}
	st = ntfsmac_journal_get(g_j, VOL_SERIAL(n), 976773167u, &e);
	CHECK(st == NTFSMAC_JOURNAL_OK || st == NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	if (st == NTFSMAC_JOURNAL_OK)
		check_entry_whole(&e);
	snprintf(reply, cap, "{\"ok\":true,\"journal\":%d}", st == NTFSMAC_JOURNAL_OK ? 1 : 0);
	return NTFSMAC_DMN_OK;
}

static ntfsmac_dmn_status op_mount(void *ctx, const char *bsd, bool ro, char *reply, size_t cap)
{
	(void)ctx;
	(void)bsd;
	(void)ro;
	ops_count(NTFSMAC_CTL_CMD_MOUNT);
	snprintf(reply, cap, "{\"ok\":true}");
	return NTFSMAC_DMN_OK;
}

static ntfsmac_dmn_status op_unmount(void *ctx, const char *bsd, bool force, char *reply,
				     size_t cap)
{
	(void)ctx;
	(void)bsd;
	(void)force;
	ops_count(NTFSMAC_CTL_CMD_UNMOUNT);
	snprintf(reply, cap, "{\"ok\":true}");
	return NTFSMAC_DMN_OK;
}

static ntfsmac_dmn_status op_doctor(void *ctx, char *reply, size_t cap)
{
	(void)ctx;
	ops_count(NTFSMAC_CTL_CMD_DOCTOR);
	snprintf(reply, cap, "{\"ok\":true}");
	return NTFSMAC_DMN_OK;
}

static ntfsmac_dmn_status op_journal_clear(void *ctx, const ntfsmac_ctl_peer *peer, const char *bsd,
					   const char *label, const char *serial, char *reply,
					   size_t cap)
{
	unsigned long s = strtoul(serial, NULL, 16);
	ntfsmac_journal_status st;

	(void)ctx;
	(void)peer;
	(void)bsd;
	(void)label;
	ops_count(NTFSMAC_CTL_CMD_JOURNAL_CLEAR);
	if (s >= 0x1000u && s < 0x1000u + SHARED_FORMS) {
		pthread_mutex_lock(&g_ops.mu);
		g_ops.shared_reached[s - 0x1000u]++;
		pthread_mutex_unlock(&g_ops.mu);
	}
	/* The same journal the volume threads are updating. */
	st = ntfsmac_journal_remove(g_j, s, 976773167u, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, next_time());
	CHECK(st == NTFSMAC_JOURNAL_OK || st == NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	snprintf(reply, cap, "{\"ok\":true,\"removed\":%d}", st == NTFSMAC_JOURNAL_OK ? 1 : 0);
	return NTFSMAC_DMN_OK;
}

static ntfsmac_dmn_status op_repair(void *ctx, const ntfsmac_ctl_peer *peer, const char *bsd,
				    const char *action, const char *label, const char *serial,
				    char *reply, size_t cap)
{
	(void)ctx;
	(void)peer;
	(void)bsd;
	(void)action;
	(void)label;
	(void)serial;
	ops_count(NTFSMAC_CTL_CMD_REPAIR);
	snprintf(reply, cap, "{\"ok\":true}");
	return NTFSMAC_DMN_OK;
}

static bool op_is_spawned(void *ctx, pid_t pid)
{
	(void)ctx;
	(void)pid;
	return false;
}

/* ---------------- system facts: read-only, so any thread may call them ---------------- */

static const ntfsmac_ctl_peer g_peer = {.uid = 501, .pid = 500, .console_user = true,
					.admin = true, .has_audit_token = true};

static int sys_peer_from_fd(void *ctx, int fd, ntfsmac_ctl_peer *out)
{
	(void)ctx;
	(void)fd;
	*out = g_peer;
	return 0;
}

static ntfsmac_ctl_sys_proc sys_proc_parent(void *ctx, pid_t pid, pid_t *ppid, pid_t *pgid)
{
	(void)ctx;
	if (pid == 500) {
		*ppid = 400;
		*pgid = 500;
		return NTFSMAC_CTL_PROC_OK;
	}
	if (pid == 400) {
		*ppid = 1;
		*pgid = 400;
		return NTFSMAC_CTL_PROC_OK;
	}
	return NTFSMAC_CTL_PROC_GONE;
}

static ntfsmac_ctl_sys_answer sys_signed(void *ctx, const ntfsmac_ctl_peer *peer)
{
	(void)ctx;
	(void)peer;
	return NTFSMAC_CTL_SYS_YES;
}

static ntfsmac_ctl_sys_answer sys_authz(void *ctx, const uint8_t form[NTFSMAC_CTL_AUTH_FORM_BYTES])
{
	(void)ctx;
	(void)form;
	return NTFSMAC_CTL_SYS_YES;
}

static ntfsmac_ctl_sys_answer sys_right(void *ctx)
{
	(void)ctx;
	return NTFSMAC_CTL_SYS_YES;
}

static ntfsmac_ctl_sys g_sys = {.ctx = NULL,
				.peer_from_fd = sys_peer_from_fd,
				.proc_parent = sys_proc_parent,
				.client_signed = sys_signed,
				.authz_verify = sys_authz,
				.right_definition = sys_right};
static ntfsmac_ctl_replay g_replay;
/* The socket door refuses journal_clear and repair; the XPC door serves only those (T-M4-14). */
static ntfsmac_ctl_env g_env = {.sys = &g_sys, .replay = &g_replay};
static ntfsmac_ctl_env g_env_xpc = {.sys = &g_sys, .replay = &g_replay,
				    .channel = NTFSMAC_CTL_CHAN_XPC};

/* The door a request line belongs to, as a client would pick it. */
static const ntfsmac_ctl_env *env_for(const char *line)
{
	return strstr(line, "\"journal_clear\"") != NULL || strstr(line, "\"repair\"") != NULL
		       ? &g_env_xpc
		       : &g_env;
}
static ntfsmac_ctl_ops g_ops_table;

/* ---------------- request builders ---------------- */

static void auth_text(char out[65], unsigned who, unsigned n)
{
	/* 64 hex digits, different for every (who, n) */
	snprintf(out, 65, "%016x%016x%016x%016x", who, n, who ^ 0xA5A5u, n ^ 0x5A5Au);
}

static void make_line(char *out, size_t cap, uint64_t *rs, unsigned who, unsigned n)
{
	char auth[65];

	switch (rnd_next(rs) % 8u) {
	case 0:
		snprintf(out, cap, "{\"cmd\":\"list\"}");
		break;
	case 1:
		snprintf(out, cap, "{\"cmd\":\"status\",\"bsd\":\"disk%us1\"}",
			 (unsigned)(rnd_next(rs) % NVOL));
		break;
	case 2:
		snprintf(out, cap, "{\"cmd\":\"mount\",\"bsd\":\"disk%us1\",\"mode\":\"ro\"}",
			 (unsigned)(rnd_next(rs) % NVOL));
		break;
	case 3:
		snprintf(out, cap, "{\"cmd\":\"unmount\",\"bsd\":\"disk%us1\"}",
			 (unsigned)(rnd_next(rs) % NVOL));
		break;
	case 4:
		snprintf(out, cap, "{\"cmd\":\"doctor\"}");
		break;
	case 5: /* privileged, fresh form: reaches the journal */
		auth_text(auth, who, n);
		snprintf(out, cap,
			 "{\"cmd\":\"journal_clear\",\"bsd\":\"disk%us1\",\"confirm_label\":\"L\","
			 "\"confirm_serial\":\"%08X\",\"auth\":\"%s\"}",
			 (unsigned)(rnd_next(rs) % NVOL), VOL_SERIAL(rnd_next(rs) % NVOL), auth);
		break;
	case 6: /* privileged without a form: refused */
		snprintf(out, cap,
			 "{\"cmd\":\"journal_clear\",\"bsd\":\"disk0s1\",\"confirm_label\":\"L\","
			 "\"confirm_serial\":\"%08X\"}",
			 VOL_SERIAL(0));
		break;
	default: /* malformed */
		snprintf(out, cap, "{\"cmd\":\"status\",\"bsd\":");
		break;
	}
}

static void check_reply(const char *r)
{
	size_t n = strlen(r);

	CHECK(strncmp(r, "{\"ok\":", 6) == 0);
	CHECK(n > 0 && r[n - 1] == '}');
	CHECK(strchr(r, '\n') == NULL);
}

/* ---------------- clients over the socket ---------------- */

static int dial(void)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	if (snprintf(sa.sun_path, sizeof sa.sun_path, "%s", g_sock) >= (int)sizeof sa.sun_path) {
		fprintf(stderr, "socket path too long\n");
		exit(2);
	}
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}

static void *client_main(void *arg)
{
	unsigned who = (unsigned)(uintptr_t)arg;
	uint64_t rs = 0x1234567ull * (who + 7u);
	unsigned seq = 0;
	int c;

	for (c = 0; c < CLIENT_CONNS; c++) {
		char req[1024], all[4096], in[16384];
		size_t have = 0, alen = 0;
		unsigned nreq = 1 + (unsigned)(rnd_next(&rs) % 4u), got = 0, k;
		long long end = mono_ms() + 15000;
		int fd = dial();

		if (fd < 0) {
			CHECK(false);
			continue;
		}
		for (k = 0; k < nreq; k++) { /* pipelined: all lines in one write */
			make_line(req, sizeof req, &rs, who, seq++);
			alen += (size_t)snprintf(all + alen, sizeof all - alen, "%s\n", req);
		}
		CHECK(send(fd, all, alen, 0) == (ssize_t)alen);
		while (got < nreq && mono_ms() < end) {
			struct pollfd p = {fd, POLLIN, 0};
			ssize_t r;
			char *nl;

			if (poll(&p, 1, 1000) <= 0)
				continue;
			r = recv(fd, in + have, sizeof in - 1 - have, 0);
			if (r <= 0)
				break;
			have += (size_t)r;
			in[have] = '\0';
			while ((nl = strchr(in, '\n')) != NULL) {
				*nl = '\0';
				check_reply(in);
				got++;
				have -= (size_t)(nl + 1 - in);
				memmove(in, nl + 1, have + 1);
			}
		}
		CHECK(got == nreq);
		close(fd);
	}
	return NULL;
}

/* ---------------- direct callers of the handler ---------------- */

/* macOS has no pthread_barrier_t: a small one with a mutex and a condition. */
typedef struct barrier {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	unsigned need, here, generation;
} barrier;

static barrier g_barrier = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NDIRECT, 0, 0};

static void barrier_wait(barrier *b)
{
	unsigned gen;

	pthread_mutex_lock(&b->mu);
	gen = b->generation;
	if (++b->here == b->need) {
		b->here = 0;
		b->generation++;
		pthread_cond_broadcast(&b->cv);
	} else {
		while (gen == b->generation)
			pthread_cond_wait(&b->cv, &b->mu);
	}
	pthread_mutex_unlock(&b->mu);
}

static void *direct_main(void *arg)
{
	unsigned who = 100u + (unsigned)(uintptr_t)arg;
	uint64_t rs = 0xABCDEFull * (who + 3u);
	char reply[NTFSMAC_CTL_REPLY_MAX];
	char line[1024];
	unsigned i;

	/* Replay: every thread sends the same fresh form at the same moment. */
	for (i = 0; i < SHARED_FORMS; i++) {
		char auth[65];

		auth_text(auth, 0xFFFFu, i);
		snprintf(line, sizeof line,
			 "{\"cmd\":\"journal_clear\",\"bsd\":\"disk0s1\",\"confirm_label\":\"L\","
			 "\"confirm_serial\":\"%08X\",\"auth\":\"%s\"}",
			 SHARED_SERIAL(i), auth);
		barrier_wait(&g_barrier);
		(void)ntfsmac_ctl_handle_line_env(env_for(line), &g_ops_table, NULL, &g_peer, line,
						  strlen(line), reply, sizeof reply);
		check_reply(reply);
	}
	/* Then random requests, as a client would send them. */
	for (i = 0; i < DIRECT_LINES; i++) {
		make_line(line, sizeof line, &rs, who, i);
		(void)ntfsmac_ctl_handle_line_env(env_for(line), &g_ops_table, NULL, &g_peer, line,
						  strlen(line), reply, sizeof reply);
		check_reply(reply);
	}
	return NULL;
}

/* ---------------- volume threads: the journal side of sessions ---------------- */

static ntfsmac_journal_status vol_update(ntfsmac_journal_entry *e)
{
	e->updated++;
	return ntfsmac_journal_update(g_j, e);
}

static void *vol_main(void *arg)
{
	unsigned i = (unsigned)(uintptr_t)arg;
	uint64_t serial = VOL_SERIAL(i);
	uint64_t rs = 0x5555ull * (i + 1u);
	int round;

	for (round = 0; round < VOL_ROUNDS; round++) {
		ntfsmac_journal_entry e = new_entry(serial);
		ntfsmac_journal_status st = ntfsmac_journal_create(g_j, &e);

		/* A leftover from an earlier round, or a clear that raced us: start clean. */
		if (st == NTFSMAC_JOURNAL_ERR_EXISTS) {
			st = ntfsmac_journal_remove(g_j, serial, 976773167u, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR,
						    next_time());
			CHECK(st == NTFSMAC_JOURNAL_OK || st == NTFSMAC_JOURNAL_ERR_NOT_FOUND);
			continue;
		}
		CHECK(st == NTFSMAC_JOURNAL_OK);
		if (st != NTFSMAC_JOURNAL_OK)
			continue;

		/* A control request may remove the entry between any two calls. */
		e.marker = NTFSMAC_JOURNAL_MARKER_SET;
		st = vol_update(&e);
		CHECK(st == NTFSMAC_JOURNAL_OK || st == NTFSMAC_JOURNAL_ERR_NOT_FOUND);
		if (st != NTFSMAC_JOURNAL_OK)
			continue;
		if (rnd_next(&rs) % 3u == 0) {
			e.state = NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
			st = vol_update(&e);
			CHECK(st == NTFSMAC_JOURNAL_OK || st == NTFSMAC_JOURNAL_ERR_NOT_FOUND);
			continue; /* left for a clear, as after a real write error */
		}
		st = ntfsmac_journal_remove(g_j, serial, 976773167u, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION,
					    next_time());
		CHECK(st == NTFSMAC_JOURNAL_OK || st == NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	}
	return NULL;
}

/* ---------------- phase 2 ---------------- */

static void phase_concurrent(void)
{
	pthread_t vol[NVOL], cl[NCLIENT], dir[NDIRECT];
	ntfsmac_ctl *ctl = NULL;
	unsigned i;

	pthread_mutex_init(&g_ops.mu, NULL);
	ntfsmac_ctl_replay_init(&g_replay);
	memset(&g_ops_table, 0, sizeof g_ops_table);
	g_ops_table.list = op_list;
	g_ops_table.status = op_status;
	g_ops_table.mount = op_mount;
	g_ops_table.unmount = op_unmount;
	g_ops_table.doctor = op_doctor;
	g_ops_table.journal_clear = op_journal_clear;
	g_ops_table.repair = op_repair;
	g_ops_table.is_spawned = op_is_spawned;

	CHECK(ntfsmac_ctl_start_env(g_sock, &g_ops_table, NULL, &g_env, &ctl) == NTFSMAC_DMN_OK);
	if (ctl == NULL)
		return;
	for (i = 0; i < NVOL; i++)
		CHECK(pthread_create(&vol[i], NULL, vol_main, (void *)(uintptr_t)i) == 0);
	for (i = 0; i < NCLIENT; i++)
		CHECK(pthread_create(&cl[i], NULL, client_main, (void *)(uintptr_t)i) == 0);
	for (i = 0; i < NDIRECT; i++)
		CHECK(pthread_create(&dir[i], NULL, direct_main, (void *)(uintptr_t)i) == 0);

	for (i = 0; i < NDIRECT; i++)
		pthread_join(dir[i], NULL);
	for (i = 0; i < NCLIENT; i++)
		pthread_join(cl[i], NULL);
	for (i = 0; i < NVOL; i++)
		pthread_join(vol[i], NULL);
	ntfsmac_ctl_stop(ctl);

	/* Replay: each shared form reached the operation exactly once. */
	for (i = 0; i < SHARED_FORMS; i++)
		CHECK(g_ops.shared_reached[i] == 1);
	/* The run did reach every kind of operation. */
	CHECK(g_ops.calls[NTFSMAC_CTL_CMD_LIST] > 0);
	CHECK(g_ops.calls[NTFSMAC_CTL_CMD_STATUS] > 0);
	CHECK(g_ops.calls[NTFSMAC_CTL_CMD_MOUNT] > 0);
	CHECK(g_ops.calls[NTFSMAC_CTL_CMD_UNMOUNT] > 0);
	CHECK(g_ops.calls[NTFSMAC_CTL_CMD_DOCTOR] > 0);
	CHECK(g_ops.calls[NTFSMAC_CTL_CMD_JOURNAL_CLEAR] > SHARED_FORMS);

	/* What is left in the journal is whole. */
	{
		unsigned skipped = 0;

		CHECK(ntfsmac_journal_list(g_j, list_check_cb, NULL, &skipped) == NTFSMAC_JOURNAL_OK);
		CHECK(skipped == 0);
	}
	printf("tsan_ctl_journal: list %d, status %d, mount %d, unmount %d, doctor %d, "
	       "journal_clear %d (each shared form reached once)\n",
	       g_ops.calls[NTFSMAC_CTL_CMD_LIST], g_ops.calls[NTFSMAC_CTL_CMD_STATUS],
	       g_ops.calls[NTFSMAC_CTL_CMD_MOUNT], g_ops.calls[NTFSMAC_CTL_CMD_UNMOUNT],
	       g_ops.calls[NTFSMAC_CTL_CMD_DOCTOR], g_ops.calls[NTFSMAC_CTL_CMD_JOURNAL_CLEAR]);
}

static void remove_tree(void)
{
	char cmd[512];

	/* Test cleanup of our own temporary directory (two levels, no symlinks). */
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
	if (system(cmd) != 0)
		fprintf(stderr, "cleanup of %s failed\n", g_dir);
}

int main(void)
{
	const char *t = getenv("TMPDIR");

	ntfsmac_log_set_level(NTFSMAC_LOG_ERROR); /* the journal logs every removal at info level */
	snprintf(g_dir, sizeof g_dir, "%s%sntfsmac-tsan-XXXXXX", t != NULL ? t : "/tmp",
		 (t != NULL && t[0] != '\0' && t[strlen(t) - 1] == '/') ? "" : "/");
	if (mkdtemp(g_dir) == NULL) {
		perror("mkdtemp");
		return 2;
	}
	snprintf(g_jdir, sizeof g_jdir, "%s/journal", g_dir);
	snprintf(g_sock, sizeof g_sock, "%s/s", g_dir);
	if (mkdir(g_jdir, 0700) != 0 || ntfsmac_journal_open(g_jdir, &g_j) != NTFSMAC_JOURNAL_OK) {
		fprintf(stderr, "cannot open the journal in %s\n", g_jdir);
		return 2;
	}

	phase_kill();
	phase_concurrent();

	ntfsmac_journal_close(g_j);
	remove_tree();
	if (check_failures() != 0) {
		fprintf(stderr, "%d check(s) failed\n", check_failures());
		return 1;
	}
	return 0;
}
