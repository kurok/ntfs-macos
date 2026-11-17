/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M4-14: the session lifecycle and the privileged operations against the real probe, volmark
 * and worker in a macOS guest (Environment B), run as root by journal_vm.sh. It does what the
 * daemon of T-M4-08 will do, with the modules of this task and the real spawn module and
 * sandbox, on an NTFS image FILE (never a host disk). What it cannot have without the wired
 * daemon is Disk Arbitration: the description of the disk (the "no other mount" part of the
 * binding, I-DMN-25 b) is a fake here, so that part is pending for T-M4-11.
 *
 *   journal_vm WORKDIR UID GID SBXDIR PROBE VOLMARK WORKER STUBDIR SETFLAGS LOGMARK [CASE]
 *
 * WORKDIR contains base.img (an NTFS image whose log was initialised with logfile_mark) and
 * img2.img (another volume), both mode 600 and root's. SBXDIR holds the root-owned sandbox
 * profiles. STUBDIR holds links to journal_stub named jstub-exit1, jstub-sector0, jstub-hang.
 * SETFLAGS and LOGMARK are tests/tools/setvolflags and logfile_mark.
 *
 * Cases (each prints PASS, FAIL or NOTE lines; exit 0 only if none failed):
 *   rw        I-DMN-22: the dirty flag is set during a rw session and clear after a clean end;
 *             the order of section 3.6 holds with the real tools; the entry is removed
 *   unclean   I-DMN-22: a worker killed in the middle leaves the flag set and the entry; the
 *             next decision is ro
 *   markfail  I-DMN-22: volmark replaced by a program that exits 1: a ro mount, marker=failed,
 *             the image is byte for byte unchanged; the same for a hang before `w`
 *   bind      I-DMN-25 a, c: another image under the same name; changed bytes of sector 0;
 *             volmark that changes sector 0 (b needs Disk Arbitration, pending)
 *   evidence  I-DMN-20: flag cleared and LSN advanced by tools, rule enabled and disabled
 *   clear     I-DMN-19 (module level): journal-clear gates on real probe verdicts
 *   repair    I-DMN-21 (module level): repair --clear-dirty on real images
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "spawn_config.h"

static int g_fails;
static uid_t g_uid;
static gid_t g_gid;
static const char *g_work, *g_sbx, *g_probe, *g_volmark, *g_worker, *g_stubdir, *g_setflags,
	*g_logmark;
static char g_image[300], g_base[300], g_image2[300], g_jdir[300];
static const char *g_open_path; /* what the fake device node opens */
static int g_log_fd = -1;

#define PASS(...) \
	do { printf("PASS: " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define FAIL(...) \
	do { printf("FAIL: " __VA_ARGS__); printf("\n"); fflush(stdout); g_fails++; } while (0)
#define NOTE(...) \
	do { printf("NOTE: " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(cond, ...) \
	do { if (cond) PASS(__VA_ARGS__); else FAIL(__VA_ARGS__); } while (0)

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000u, (long)(ms % 1000u) * 1000000L };

	nanosleep(&ts, NULL);
}

static int run(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int run(const char *fmt, ...)
{
	char cmd[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	fflush(stdout);
	return system(cmd);
}

/* ---------------- the real session facts, with the device node replaced by an image ---------------- */

static ntfsmac_session_sys_real g_real;
static bool g_apple_mount;      /* the fake description says Apple's driver has the disk */
static bool g_other_mount;      /* ... another driver */

static int vm_open(void *ctx, const char *bsd, bool writable)
{
	(void)ctx;
	(void)bsd;
	return open(g_open_path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
}

static ntfsmac_dmn_status vm_describe(void *ctx, const char *bsd, ntfsmac_disk_info *out)
{
	(void)ctx;
	memset(out, 0, sizeof(*out));
	snprintf(out->bsd_name, sizeof(out->bsd_name), "%s", bsd);
	snprintf(out->media_path, sizeof(out->media_path), "journalvm-image");
	if (g_apple_mount || g_other_mount) {
		out->has_volume_path = true;
		snprintf(out->volume_kind, sizeof(out->volume_kind), g_apple_mount ? "ntfs" : "ufsd_NTFS");
	}
	return NTFSMAC_DMN_OK;
}

static ntfsmac_session_sys g_sys;

static void sys_init(void)
{
	ntfsmac_session_sys_real_init(&g_real, NULL);
	g_sys = g_real.sys;
	g_sys.open_device = vm_open;
	g_sys.describe = vm_describe;
	g_sys.ctx = &g_real;
}

static void spawn_cfg(const char *volmark)
{
	ntfsmac_spawn_config c;

	ntfsmac_spawn_config_default(&c);
	c.probe_path = g_probe;
	c.volmark_path = volmark != NULL ? volmark : g_volmark;
	c.worker_path = g_worker;
	c.child_stderr_fd = g_log_fd;
	c.sandbox_dir = g_sbx;
	c.sandbox_owner = 0;
	c.volmark_write_wait_s = 10;
	ntfsmac_spawn_config_set(&c);
}

/* ---------------- images ---------------- */

static void reset_image(void)
{
	run("cp '%s' '%s' && chmod 600 '%s'", g_base, g_image, g_image);
	g_open_path = g_image;
	g_apple_mount = g_other_mount = false;
}

static bool sha_of(const char *path, char *out, size_t cap)
{
	char cmd[400];
	FILE *f;
	bool ok;

	snprintf(cmd, sizeof(cmd), "/usr/bin/shasum -a 256 '%s'", path);
	f = popen(cmd, "r");
	if (f == NULL)
		return false;
	ok = fgets(out, (int)cap, f) != NULL;
	pclose(f);
	if (ok)
		out[strcspn(out, " ")] = '\0';
	return ok;
}

static bool same_sha(const char *path, const char *want)
{
	char now[128];

	return sha_of(path, now, sizeof(now)) && strcmp(now, want) == 0;
}

/* Bytes that differ between two copies of the image, listed only to be checked against windows. */
typedef struct window {
	off_t from, to;
} window;

static bool read_all(const char *path, unsigned char **buf, off_t *len)
{
	struct stat st;
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0 || fstat(fd, &st) != 0) {
		if (fd >= 0)
			close(fd);
		return false;
	}
	*buf = malloc((size_t)st.st_size);
	if (*buf == NULL) {
		close(fd);
		return false;
	}
	n = pread(fd, *buf, (size_t)st.st_size, 0);
	close(fd);
	*len = st.st_size;
	return n == st.st_size;
}

/* MFT record 3 ($Volume) in $MFT and in $MFTMirr, from the boot sector of `buf`. */
static bool volume_windows(const unsigned char *b, window w[2])
{
	unsigned bps = (unsigned)b[0x0B] | ((unsigned)b[0x0C] << 8);
	unsigned spc = b[0x0D];
	uint64_t mft = 0, mir = 0;
	int8_t cpr = (int8_t)b[0x40];
	unsigned rec;
	unsigned i;

	for (i = 0; i < 8; i++) {
		mft |= (uint64_t)b[0x30 + i] << (8 * i);
		mir |= (uint64_t)b[0x38 + i] << (8 * i);
	}
	if (bps == 0 || spc == 0 || cpr >= 0)
		return false;
	rec = 1u << (unsigned)(-cpr);
	w[0].from = (off_t)(mft * spc * bps + 3u * rec);
	w[0].to = w[0].from + rec;
	w[1].from = (off_t)(mir * spc * bps + 3u * rec);
	w[1].to = w[1].from + rec;
	return true;
}

/* Every byte that differs from `before` is inside one of the two windows; returns how many differ. */
static long diff_outside(const char *before, const char *after, long *inside)
{
	unsigned char *a = NULL, *b = NULL;
	off_t la = 0, lb = 0, i;
	window w[2];
	long outside = 0;

	*inside = 0;
	if (!read_all(before, &a, &la) || !read_all(after, &b, &lb) || la != lb || !volume_windows(a, w)) {
		free(a);
		free(b);
		return -1;
	}
	for (i = 0; i < la; i++) {
		if (a[i] == b[i])
			continue;
		if ((i >= w[0].from && i < w[0].to) || (i >= w[1].from && i < w[1].to))
			(*inside)++;
		else
			outside++;
	}
	free(a);
	free(b);
	return outside;
}

/* ---------------- probe and attach ---------------- */

static bool probe_now(ntfsmac_probe_verdict *v)
{
	ntfsmac_probe_run pr;
	ntfsmac_dmn_status st;
	int fd = vm_open(NULL, "disk99s1", false);

	if (fd < 0)
		return false;
	memset(&pr, 0, sizeof(pr));
	pr.device_fd = fd;
	pr.timeout_s = 20;
	st = ntfsmac_spawn_probe(&pr, v);
	close(fd);
	return st == NTFSMAC_DMN_OK;
}

typedef struct attach {
	ntfsmac_session_begin_in in;
	ntfsmac_probe_verdict v;
	ntfsmac_journal *j;
	int a;
} attach;

static bool attach_prepare(attach *at, ntfsmac_journal *j)
{
	memset(at, 0, sizeof(*at));
	at->j = j;
	at->a = vm_open(NULL, "disk99s1", false);
	if (at->a < 0 || g_sys.read_block0(g_sys.ctx, at->a, at->in.block0) != 0)
		return false;
	if (!probe_now(&at->v))
		return false;
	at->in.bsd = "disk99s1";
	at->in.verdict = &at->v;
	at->in.probe_fd = at->a;
	snprintf(at->in.media_path, sizeof(at->in.media_path), "journalvm-image");
	return true;
}

static const char *state_name(const ntfsmac_probe_verdict *v)
{
	static const char *const n[] = {
#define X(id, str) str,
		NTFSMAC_PROBE_STATE_LIST(X)
#undef X
	};

	return (unsigned)v->state < NTFSMAC_PROBE_STATE_COUNT_ ? n[v->state] : "?";
}

static const char *probe_state_now(void)
{
	static ntfsmac_probe_verdict v;

	if (!probe_now(&v))
		return "probe-failed";
	return state_name(&v);
}

static bool entry_get(ntfsmac_journal *j, uint64_t serial, ntfsmac_journal_entry *e)
{
	return ntfsmac_journal_get(j, serial, e) == NTFSMAC_JOURNAL_OK;
}

/* ---------------- mount, workload, end (the daemon's sequence of section 4.5) ---------------- */

static bool mounted_now(const char *mp)
{
	struct statfs *fs;
	int n, i;
	bool found = false;

	n = getfsstat(NULL, 0, MNT_NOWAIT);
	if (n <= 0)
		return false;
	fs = calloc((size_t)n + 8, sizeof(*fs));
	if (fs == NULL)
		return false;
	n = getfsstat(fs, (int)(((size_t)n + 8) * sizeof(*fs)), MNT_NOWAIT);
	for (i = 0; i < n; i++)
		if (strcmp(fs[i].f_mntonname, mp) == 0)
			found = true;
	free(fs);
	return found;
}

static int unmount_bounded(const char *mp, int flags, unsigned limit_s)
{
	pid_t p = fork();
	int st;
	uint64_t end;

	if (p < 0)
		return -1;
	if (p == 0)
		_exit(unmount(mp, flags) == 0 ? 0 : (errno > 0 && errno < 200 ? errno : 1));
	end = now_ms() + limit_s * 1000u;
	for (;;) {
		pid_t r = waitpid(p, &st, WNOHANG);

		if (r == p)
			return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
		if (now_ms() >= end) {
			int i;

			kill(p, SIGKILL);
			for (i = 0; i < 100 && waitpid(p, &st, WNOHANG) != p; i++)
				sleep_ms(20);
			return -2;
		}
		sleep_ms(20);
	}
}

static int user_write_test(const char *mp)
{
	pid_t p = fork();
	int st;
	uint64_t end;

	if (p < 0)
		return -1;
	if (p == 0) {
		char path[256], buf[4096];
		gid_t groups[1] = { g_gid };
		int fd, i;

		if (setgroups(1, groups) != 0 || setgid(g_gid) != 0 || setuid(g_uid) != 0)
			_exit(10);
		memset(buf, 'j', sizeof(buf));
		snprintf(path, sizeof(path), "%s/journal_vm.bin", mp);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			_exit(11);
		for (i = 0; i < 64; i++)
			if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
				_exit(12);
		if (fsync(fd) != 0 || close(fd) != 0)
			_exit(13);
		_exit(0);
	}
	end = now_ms() + 20000u;
	for (;;) {
		pid_t r = waitpid(p, &st, WNOHANG);

		if (r == p)
			return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
		if (now_ms() >= end) {
			int i;

			kill(p, SIGKILL);
			for (i = 0; i < 100 && waitpid(p, &st, WNOHANG) != p; i++)
				sleep_ms(20);
			return -2;
		}
		sleep_ms(20);
	}
}

typedef struct mnt {
	ntfsmac_child child;
	char mp[128];
	int rd;
	ntfsmac_errwatch w;
	bool mounted, mounted_ro;
} mnt;

/* Spawn the worker with the descriptor of the session; the channel is ours. */
static bool mount_session(mnt *m, ntfsmac_session *s, const char *tag)
{
	ntfsmac_worker_run run;
	int wr = -1;
	ntfsmac_dmn_status st;

	memset(m, 0, sizeof(*m));
	m->rd = -1;
	ntfsmac_errwatch_init(&m->w);
	snprintf(m->mp, sizeof(m->mp), "/Volumes/journalvm%s", tag);
	if (mkdir(m->mp, 0700) != 0 || chown(m->mp, g_uid, g_gid) != 0) {
		FAIL("%s: cannot make %s: %s", tag, m->mp, strerror(errno));
		return false;
	}
	if (ntfsmac_errwatch_pipe_open(&m->rd, &wr) != NTFSMAC_DMN_OK) {
		FAIL("%s: cannot make the channel", tag);
		rmdir(m->mp);
		return false;
	}
	memset(&run, 0, sizeof(run));
	run.device_fd = ntfsmac_session_device_fd(s);
	run.errfd_write = wr;
	run.mount_point = m->mp;
	run.read_only = ntfsmac_session_mode(s) != NTFSMAC_VMODE_RW;
	run.console_uid = g_uid;
	run.console_gid = g_gid;
	run.mount_wait_s = 30;
	st = ntfsmac_spawn_worker(&run, &m->child, &m->mounted_ro);
	close(wr);
	if (st != NTFSMAC_DMN_OK) {
		FAIL("%s: the worker did not mount (%s)", tag, ntfsmac_dmn_status_name(st));
		close(m->rd);
		rmdir(m->mp);
		return false;
	}
	m->mounted = true;
	return true;
}

/* Unmount (or kill), reap, drain: what the daemon does before session_end(). */
static void settle(mnt *m, ntfsmac_session *s, const char *tag, bool kill_it, bool workload,
		   bool *unmount_first, int *exit_status)
{
	bool exited = false;
	int i, rc;

	*exit_status = -1;
	*unmount_first = false;
	for (i = 0; i < 40 && (m->w.flags & (NTFSMAC_EW_SAW_S_RW | NTFSMAC_EW_SAW_S_RO)) == 0u; i++) {
		(void)ntfsmac_errwatch_pump(&m->w, m->rd);
		sleep_ms(50);
	}
	if (workload && !kill_it) {
		rc = user_write_test(m->mp);
		NOTE("%s: the user's write test returned %d", tag, rc);
		(void)ntfsmac_errwatch_pump(&m->w, m->rd);
		if ((m->w.flags & (NTFSMAC_EW_SAW_E | NTFSMAC_EW_PROTOCOL_ERROR)) != 0u)
			(void)ntfsmac_session_note_write_error(s);
	}
	if (kill_it) {
		(void)ntfsmac_child_kill_group(&m->child);
		rc = unmount_bounded(m->mp, MNT_FORCE, 30);
		NOTE("%s: forced unmount after the kill gave %d", tag, rc);
	} else {
		rc = unmount_bounded(m->mp, 0, 30);
		CHECK(rc == 0, "%s: unmount(2) (code %d)", tag, rc);
		*unmount_first = rc == 0;
		if (rc != 0)
			NOTE("%s: forced unmount gave %d", tag, unmount_bounded(m->mp, MNT_FORCE, 30));
	}
	for (i = 0; i < 600 && !exited; i++) {
		if (ntfsmac_child_poll(&m->child, &exited, exit_status) != NTFSMAC_DMN_OK)
			break;
		if (!exited)
			sleep_ms(50);
	}
	if (!exited) {
		(void)ntfsmac_child_kill_group(&m->child);
		*exit_status = -1;
	}
	(void)ntfsmac_errwatch_drain(&m->w, m->rd, NTFSMAC_ERRWATCH_EOF_WAIT_MS);
	CHECK(!mounted_now(m->mp), "%s: the mount is gone", tag);
	rmdir(m->mp);
	close(m->rd);
}

/* ---------------- cases ---------------- */

#define ENTRY_SERIAL(v) ((v).serial)

/* A rw attach: probe, decide, begin; returns the session or NULL. */
static ntfsmac_session *rw_attach(ntfsmac_journal *j, const ntfsmac_session_config *cfg,
				  attach *at, ntfsmac_session_start *res, ntfsmac_dmn_status *st)
{
	ntfsmac_session *s = NULL;
	ntfsmac_session_decision d;

	if (!attach_prepare(at, j)) {
		FAIL("attach: could not open the image or run the probe");
		return NULL;
	}
	if (ntfsmac_session_decide(cfg, j, &at->v, false, false, (int64_t)time(NULL), NULL, NULL, &d) != NTFSMAC_DMN_OK) {
		FAIL("attach: decide failed");
		return NULL;
	}
	at->in.decision = d.policy.decision;
	*st = ntfsmac_session_begin(&g_sys, cfg, j, &at->in, &s, res);
	return s;
}

static void case_rw(void)
{
	ntfsmac_journal *j;
	ntfsmac_session_config cfg;
	attach at;
	ntfsmac_session_start res;
	ntfsmac_session_end_result er;
	ntfsmac_dmn_status st = NTFSMAC_DMN_OK;
	ntfsmac_session *s;
	ntfsmac_journal_entry e;
	mnt m;
	bool um;
	int xs;
	long inside, outside;
	char base_copy[320];

	printf("== rw: marker set before the worker, cleared and the entry removed after a clean end (I-DMN-22)\n");
	reset_image();
	run("rm -rf '%s' && cp '%s' '%s.pre'", g_jdir, g_image, g_image);
	snprintf(base_copy, sizeof(base_copy), "%s.pre", g_image);
	CHECK(ntfsmac_journal_open(g_jdir, &j) == NTFSMAC_JOURNAL_OK, "the journal opens");
	memset(&cfg, 0, sizeof(cfg));
	spawn_cfg(NULL);
	CHECK(strcmp(probe_state_now(), "clean") == 0, "the image probes clean before the session");

	s = rw_attach(j, &cfg, &at, &res, &st);
	CHECK(s != NULL && st == NTFSMAC_DMN_OK && res.mode == NTFSMAC_VMODE_RW,
	      "begin gave a rw session (%s, mode %d)", ntfsmac_dmn_status_name(st), (int)res.mode);
	if (s == NULL) {
		ntfsmac_journal_close(j);
		return;
	}
	CHECK(entry_get(j, ENTRY_SERIAL(at.v), &e) && e.marker == NTFSMAC_JOURNAL_MARKER_SET &&
		      e.state == NTFSMAC_JOURNAL_STATE_OPEN,
	      "the journal says marker=set, state=open before the worker starts");
	/* the flag is on the disk now: a fresh probe through another descriptor sees it */
	CHECK(strcmp(probe_state_now(), "dirty_flag") == 0, "the image has VOLUME_IS_DIRTY set (probe: dirty_flag)");
	if (mount_session(&m, s, "rw")) {
		CHECK(!m.mounted_ro, "the worker mounted read/write");
		settle(&m, s, "rw", false, true, &um, &xs);
		CHECK(ntfsmac_session_end(s, &m.w, um, xs, &er) == NTFSMAC_DMN_OK, "session_end ran");
		CHECK(er.report == NTFSMAC_SREPORT_CLEAN || er.report == NTFSMAC_SREPORT_CLEAN_NO_FLUSH,
		      "the report is a clean end (%d)", (int)er.report);
		CHECK(er.entry_removed && !entry_get(j, ENTRY_SERIAL(at.v), &e), "the entry was removed (path 1)");
	} else {
		ntfsmac_session_free(s);
	}
	ntfsmac_session_free(s);
	CHECK(strcmp(probe_state_now(), "clean") == 0, "the flag is clear after the clean eject (probe: clean)");
	/* only MFT record 3 and its mirror (the flag, the update sequence numbers) plus the user's file changed */
	outside = diff_outside(base_copy, g_image, &inside);
	NOTE("bytes changed: %ld in the $Volume record and its mirror, %ld elsewhere (the user's file, $MFT, bitmaps)", inside, outside);
	ntfsmac_journal_close(j);
	run("rm -f '%s'", base_copy);
}

static void case_unclean(void)
{
	ntfsmac_journal *j;
	ntfsmac_session_config cfg;
	attach at, at2;
	ntfsmac_session_start res;
	ntfsmac_session_end_result er;
	ntfsmac_dmn_status st = NTFSMAC_DMN_OK;
	ntfsmac_session *s;
	ntfsmac_journal_entry e;
	ntfsmac_session_decision d;
	mnt m;
	bool um;
	int xs;

	printf("== unclean: the worker is killed in the middle (I-DMN-22, the force-detach part)\n");
	reset_image();
	run("rm -rf '%s'", g_jdir);
	CHECK(ntfsmac_journal_open(g_jdir, &j) == NTFSMAC_JOURNAL_OK, "the journal opens");
	memset(&cfg, 0, sizeof(cfg));
	spawn_cfg(NULL);
	s = rw_attach(j, &cfg, &at, &res, &st);
	CHECK(s != NULL && res.mode == NTFSMAC_VMODE_RW, "a rw session");
	if (s == NULL) {
		ntfsmac_journal_close(j);
		return;
	}
	if (mount_session(&m, s, "unclean")) {
		settle(&m, s, "unclean", true, false, &um, &xs);
		CHECK(ntfsmac_session_end(s, &m.w, um, xs, &er) == NTFSMAC_DMN_OK && er.report == NTFSMAC_SREPORT_UNCLEAN,
		      "the end is unclean (report %d)", (int)er.report);
		CHECK(!er.entry_removed && entry_get(j, ENTRY_SERIAL(at.v), &e) && e.marker == NTFSMAC_JOURNAL_MARKER_SET,
		      "the entry and marker=set stay");
	}
	ntfsmac_session_free(s);
	CHECK(strcmp(probe_state_now(), "dirty_flag") == 0, "the flag stays on the disk (probe: dirty_flag)");
	/* the next decision for this volume is ro, with no way to rw but journal-clear or repair */
	CHECK(attach_prepare(&at2, j), "a second attach: probe");
	CHECK(ntfsmac_session_decide(&cfg, j, &at2.v, false, false, (int64_t)time(NULL), NULL, NULL, &d) == NTFSMAC_DMN_OK &&
		      d.policy.decision == NTFSMAC_POLICY_RO,
	      "the next decision is ro");
	g_sys.close_fd(g_sys.ctx, at2.a);
	ntfsmac_journal_close(j);
}

static void case_markfail(void)
{
	static const struct {
		const char *stub;
		const char *what;
		ntfsmac_session_reason reason;
	} cases[] = {
		{"jstub-exit1", "volmark exits 1", NTFSMAC_SREASON_MARKER_FAILED},
		{"jstub-hang", "volmark says nothing for 20 s", NTFSMAC_SREASON_MARKER_FAILED},
	};
	size_t i;

	printf("== markfail: volmark replaced by a program that fails (I-DMN-22)\n");
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		ntfsmac_journal *j;
		ntfsmac_session_config cfg;
		attach at;
		ntfsmac_session_start res;
		ntfsmac_session_end_result er;
		ntfsmac_dmn_status st = NTFSMAC_DMN_OK;
		ntfsmac_session *s;
		ntfsmac_journal_entry e;
		mnt m;
		bool um;
		int xs;
		char sha[128], stub[300];

		reset_image();
		run("rm -rf '%s'", g_jdir);
		sha_of(g_image, sha, sizeof(sha));
		snprintf(stub, sizeof(stub), "%s/%s", g_stubdir, cases[i].stub);
		spawn_cfg(stub);
		CHECK(ntfsmac_journal_open(g_jdir, &j) == NTFSMAC_JOURNAL_OK, "%s: the journal opens", cases[i].what);
		memset(&cfg, 0, sizeof(cfg));
		cfg.volmark_timeout_s = 3; /* the 20 s of production, shortened for the hang */
		s = rw_attach(j, &cfg, &at, &res, &st);
		CHECK(s != NULL && st == NTFSMAC_DMN_OK && res.mode == NTFSMAC_VMODE_RO && res.reason == cases[i].reason,
		      "%s: the session is ro, reason %d", cases[i].what, (int)res.reason);
		if (s == NULL) {
			ntfsmac_journal_close(j);
			continue;
		}
		CHECK(entry_get(j, ENTRY_SERIAL(at.v), &e) && e.marker == NTFSMAC_JOURNAL_MARKER_FAILED,
		      "%s: the entry says marker=failed", cases[i].what);
		CHECK(!ntfsmac_session_owns_entry(s) || ntfsmac_session_mode(s) == NTFSMAC_VMODE_RO, "%s: no rw descriptor", cases[i].what);
		if (mount_session(&m, s, "markfail")) {
			CHECK(m.mounted_ro, "%s: the worker mounted read-only", cases[i].what);
			settle(&m, s, "markfail", false, false, &um, &xs);
			CHECK(ntfsmac_session_end(s, &m.w, um, xs, &er) == NTFSMAC_DMN_OK && er.report == NTFSMAC_SREPORT_RO_CLOSED,
			      "%s: the ro session closes", cases[i].what);
		}
		ntfsmac_session_free(s);
		CHECK(same_sha(g_image, sha), "%s: the image is byte for byte unchanged", cases[i].what);
		ntfsmac_journal_close(j);
	}
	spawn_cfg(NULL);
}

static void case_bind(void)
{
	ntfsmac_journal *j;
	ntfsmac_session_config cfg;
	attach at;
	ntfsmac_session_start res;
	ntfsmac_dmn_status st = NTFSMAC_DMN_OK;
	ntfsmac_session *s;
	char sha1[128], sha2[128], stub[300];
	int fd;
	unsigned char b = 0;

	printf("== bind: the descriptor must be the volume that was probed (I-DMN-25 a, c)\n");
	/* (a) another image answers under the same name after the probe */
	reset_image();
	run("rm -rf '%s'", g_jdir);
	sha_of(g_image, sha1, sizeof(sha1));
	sha_of(g_image2, sha2, sizeof(sha2));
	spawn_cfg(NULL);
	CHECK(ntfsmac_journal_open(g_jdir, &j) == NTFSMAC_JOURNAL_OK, "the journal opens");
	memset(&cfg, 0, sizeof(cfg));
	CHECK(attach_prepare(&at, j), "image X: probe");
	g_open_path = g_image2; /* the swap: every open from now on is image Y */
	at.in.decision = NTFSMAC_POLICY_RW;
	st = ntfsmac_session_begin(&g_sys, &cfg, j, &at.in, &s, &res);
	CHECK(st == NTFSMAC_DMN_E_DENIED && s == NULL && res.failed == NTFSMAC_VREASON_BINDING,
	      "(a) another device under the same name: Failed, binding (%s)", ntfsmac_dmn_status_name(st));
	CHECK(same_sha(g_image2, sha2), "(a) image Y is unchanged");
	CHECK(same_sha(g_image, sha1), "(a) image X is unchanged");
	CHECK(ntfsmac_session_device_fd(NULL) == -1, "(a) no descriptor kept");
	ntfsmac_journal_close(j);

	/* (a2) the same device, but the first 4096 bytes changed after the probe */
	reset_image();
	run("rm -rf '%s'", g_jdir);
	CHECK(ntfsmac_journal_open(g_jdir, &j) == NTFSMAC_JOURNAL_OK, "the journal opens");
	CHECK(attach_prepare(&at, j), "image X: probe");
	fd = open(g_image, O_RDWR);
	if (fd >= 0 && pread(fd, &b, 1, 0x70) == 1) {
		b ^= 0xFF;
		(void)pwrite(fd, &b, 1, 0x70); /* the test changes sector 0 itself, as a swapped disk would differ */
	}
	if (fd >= 0)
		close(fd);
	sha_of(g_image, sha1, sizeof(sha1));
	at.in.decision = NTFSMAC_POLICY_RW;
	st = ntfsmac_session_begin(&g_sys, &cfg, j, &at.in, &s, &res);
	CHECK(st == NTFSMAC_DMN_E_DENIED && s == NULL, "(a2) other bytes in sector 0: Failed (%s)", ntfsmac_dmn_status_name(st));
	CHECK(same_sha(g_image, sha1), "(a2) nothing was written after the change");
	ntfsmac_journal_close(j);

	/* (c) volmark that changes sector 0 */
	reset_image();
	run("rm -rf '%s'", g_jdir);
	snprintf(stub, sizeof(stub), "%s/jstub-sector0", g_stubdir);
	spawn_cfg(stub);
	CHECK(ntfsmac_journal_open(g_jdir, &j) == NTFSMAC_JOURNAL_OK, "the journal opens");
	s = rw_attach(j, &cfg, &at, &res, &st);
	/*
	 * The stub really wrote sector 0, so the fresh read-only descriptor of the fallback no longer
	 * matches the bytes read before the probe either: the volume is Failed ("ro or Failed",
	 * I-DMN-25 c). Either way no writable descriptor survives and the entry says marker=failed.
	 */
	CHECK((st == NTFSMAC_DMN_E_DENIED && s == NULL && res.failed == NTFSMAC_VREASON_BINDING) ||
		      (st == NTFSMAC_DMN_OK && s != NULL && res.mode == NTFSMAC_VMODE_RO),
	      "(c) volmark changed sector 0: Failed or ro, never rw (status %s, mode %d)", ntfsmac_dmn_status_name(st),
	      (int)res.mode);
	{
		ntfsmac_journal_entry e;

		CHECK(s == NULL || ntfsmac_session_mode(s) != NTFSMAC_VMODE_RW, "(c) no rw descriptor kept");
		CHECK(entry_get(j, ENTRY_SERIAL(at.v), &e) && e.marker == NTFSMAC_JOURNAL_MARKER_FAILED, "(c) the entry says marker=failed");
		ntfsmac_session_free(s);
	}
	ntfsmac_journal_close(j);
	spawn_cfg(NULL);

	/* (b) Apple's driver has the disk: the real description needs Disk Arbitration (T-M4-11) */
	NOTE("(b) the description of the disk is a fake in this driver: pending for the wired daemon (T-M4-11)");
	reset_image();
	run("rm -rf '%s'", g_jdir);
	CHECK(ntfsmac_journal_open(g_jdir, &j) == NTFSMAC_JOURNAL_OK, "the journal opens");
	g_apple_mount = true;
	sha_of(g_image, sha1, sizeof(sha1));
	s = rw_attach(j, &cfg, &at, &res, &st);
	CHECK(st == NTFSMAC_DMN_OK && s != NULL && res.mode == NTFSMAC_VMODE_RO && res.reason == NTFSMAC_SREASON_OTHER_DRIVER,
	      "(b, fake description) Apple's driver has it: ro with no write descriptor");
	ntfsmac_session_free(s);
	g_apple_mount = false;
	g_other_mount = true;
	s = rw_attach(j, &cfg, &at, &res, &st);
	CHECK(st == NTFSMAC_DMN_E_DENIED && s == NULL, "(b, fake description) another driver has it: Failed");
	CHECK(same_sha(g_image, sha1), "(b) the image is unchanged");
	ntfsmac_journal_close(j);
	g_other_mount = false;
}

/* ---------------- priv facts ---------------- */

static bool g_vol_mounted, g_vol_rw, g_live;
static uint64_t g_live_id;

static bool reg_volume(void *ctx, const char *bsd, ntfsmac_priv_volume *out)
{
	(void)ctx;
	(void)bsd;
	out->mounted = g_vol_mounted;
	out->mounted_rw = g_vol_rw;
	return true;
}

static bool reg_live(void *ctx, uint64_t id)
{
	(void)ctx;
	return g_live && id == g_live_id;
}

static ntfsmac_priv_sys_real g_psys;
static ntfsmac_priv g_priv;

static void priv_init(ntfsmac_journal *j)
{
	ntfsmac_priv_sys_real_init(&g_psys, &g_sys, reg_volume, reg_live, NULL);
	g_priv.sys = &g_psys.sys;
	g_priv.journal = j;
}

static ntfsmac_dmn_status priv_jc(const char *label, const char *serial, char *reply, size_t cap)
{
	ntfsmac_ctl_peer p;

	memset(&p, 0, sizeof(p));
	p.uid = g_uid;
	p.admin = true;
	return ntfsmac_priv_journal_clear(&g_priv, &p, "disk99s1", label, serial, reply, cap);
}

static ntfsmac_dmn_status priv_rp(const char *label, const char *serial, char *reply, size_t cap)
{
	ntfsmac_ctl_peer p;

	memset(&p, 0, sizeof(p));
	p.uid = g_uid;
	p.admin = true;
	return ntfsmac_priv_repair(&g_priv, &p, "disk99s1", "clear_dirty", label, serial, reply, cap);
}

static void serial8(const ntfsmac_probe_verdict *v, char out[16])
{
	snprintf(out, 16, "%08X", (unsigned)(v->serial & 0xFFFFFFFFu));
}

/*
 * Make an unclean rw session: entry marker=set, flag on the disk, LSN as at the start. No worker
 * is needed for that state (the kill of a real worker is case `unclean`), and a forced unmount
 * after a kill can take longer than 30 s on macFUSE (T-M4-01), which would only add noise here.
 */
static bool make_unclean(ntfsmac_journal *j, attach *at)
{
	ntfsmac_session_config cfg;
	ntfsmac_session_start res;
	ntfsmac_session_end_result er;
	ntfsmac_errwatch w;
	ntfsmac_dmn_status st = NTFSMAC_DMN_OK;
	ntfsmac_session *s;

	memset(&cfg, 0, sizeof(cfg));
	s = rw_attach(j, &cfg, at, &res, &st);
	if (s == NULL || res.mode != NTFSMAC_VMODE_RW) {
		ntfsmac_session_free(s);
		return false;
	}
	ntfsmac_errwatch_init(&w);
	ntfsmac_errwatch_eof(&w); /* a channel with no record: silence is not a clean end */
	(void)ntfsmac_session_end(s, &w, false, -1, &er);
	ntfsmac_session_free(s);
	return er.report == NTFSMAC_SREPORT_UNCLEAN;
}

static void case_evidence(void)
{
	ntfsmac_journal *j;
	ntfsmac_session_config cfg;
	attach at, at2;
	ntfsmac_session_decision d;
	ntfsmac_journal_entry e;
	int variant;

	printf("== evidence: the external-check rule with real flags and a real log sequence number (I-DMN-20)\n");
	for (variant = 0; variant < 3; variant++) {
		const char *what = variant == 0 ? "flag cleared and LSN advanced, rule on"
				  : variant == 1 ? "flag cleared and LSN advanced, rule off"
						 : "flag cleared, LSN unchanged, rule on";
		bool rule = variant != 1;

		reset_image();
		run("rm -rf '%s'", g_jdir);
		spawn_cfg(NULL);
		CHECK(ntfsmac_journal_open(g_jdir, &j) == NTFSMAC_JOURNAL_OK, "%s: the journal opens", what);
		CHECK(make_unclean(j, &at), "%s: an unclean rw session", what);
		/* Environment A's tools: another system cleared the flag and mounted the volume */
		CHECK(run("'%s' '%s' --clear DIRTY >/dev/null", g_setflags, g_image) == 0, "%s: setvolflags", what);
		if (variant != 2)
			CHECK(run("'%s' '%s' --bump-lsn >/dev/null", g_logmark, g_image) == 0, "%s: logfile_mark --bump-lsn", what);
		memset(&cfg, 0, sizeof(cfg));
		cfg.evidence_rule_enabled = rule;
		CHECK(attach_prepare(&at2, j), "%s: probe", what);
		CHECK(at2.v.state == NTFSMAC_PROBE_STATE_CLEAN, "%s: the probe says clean", what);
		CHECK(ntfsmac_session_decide(&cfg, j, &at2.v, false, false, (int64_t)time(NULL), NULL, NULL, &d) == NTFSMAC_DMN_OK &&
			      d.policy.decision == NTFSMAC_POLICY_RO,
		      "%s: this attach is ro", what);
		if (variant == 0) {
			CHECK(d.evidence_removed && !entry_get(j, ENTRY_SERIAL(at2.v), &e), "%s: the entry was removed", what);
			CHECK(ntfsmac_session_decide(&cfg, j, &at2.v, false, false, (int64_t)time(NULL) + 2, NULL, NULL, &d) == NTFSMAC_DMN_OK &&
				      d.policy.decision == NTFSMAC_POLICY_RW,
			      "%s: the next mount is rw", what);
		} else {
			CHECK(!d.evidence_removed && entry_get(j, ENTRY_SERIAL(at2.v), &e), "%s: the entry is kept", what);
		}
		g_sys.close_fd(g_sys.ctx, at2.a);
		ntfsmac_journal_close(j);
	}
}

static void case_clear(void)
{
	ntfsmac_journal *j;
	attach at, at2;
	char reply[512], ser[16];
	ntfsmac_journal_entry e;
	ntfsmac_session_config cfg;
	ntfsmac_session_decision d;
	char label[80];

	printf("== clear: journal-clear gates with real probes (I-DMN-19, module level; the peer checks are U-CTL-*)\n");
	reset_image();
	run("rm -rf '%s'", g_jdir);
	spawn_cfg(NULL);
	CHECK(ntfsmac_journal_open(g_jdir, &j) == NTFSMAC_JOURNAL_OK, "the journal opens");
	priv_init(j);
	CHECK(make_unclean(j, &at), "an unclean rw session leaves an entry");
	serial8(&at.v, ser);
	snprintf(label, sizeof(label), "%s", at.v.label);

	CHECK(priv_jc(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED && strstr(reply, "probe_not_clean") != NULL,
	      "refused while the flag is still set (%s)", reply);
	CHECK(run("'%s' '%s' --clear DIRTY >/dev/null", g_setflags, g_image) == 0, "setvolflags: clear DIRTY");
	CHECK(priv_jc("wrong label", ser, reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED && strstr(reply, "confirmation") != NULL,
	      "refused with a wrong label");
	CHECK(priv_jc(label, "00000000", reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED && strstr(reply, "confirmation") != NULL,
	      "refused with a wrong serial");
	g_vol_mounted = g_vol_rw = true;
	CHECK(priv_jc(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED && strstr(reply, "mounted_rw") != NULL,
	      "refused while mounted read/write");
	g_vol_rw = false; /* mounted read-only is fine */
	g_live = true;
	CHECK(entry_get(j, at.v.serial, &e), "the entry is still there");
	g_live_id = e.session_id;
	CHECK(priv_jc(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED && strstr(reply, "live_session") != NULL,
	      "refused while a live session (a clone on another disk) owns the entry");
	g_live = false;
	CHECK(entry_get(j, at.v.serial, &e), "every refusal left the entry");
	CHECK(priv_jc(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_OK && strstr(reply, "lost_flush") != NULL,
	      "accepted on a clean probe, with the lost-flush warning (LSN unchanged): %s", reply);
	CHECK(!entry_get(j, at.v.serial, &e), "the entry moved to the tombstones");
	g_vol_mounted = false;
	memset(&cfg, 0, sizeof(cfg));
	CHECK(attach_prepare(&at2, j), "probe");
	CHECK(ntfsmac_session_decide(&cfg, j, &at2.v, false, false, (int64_t)time(NULL), NULL, NULL, &d) == NTFSMAC_DMN_OK &&
		      d.policy.decision == NTFSMAC_POLICY_RW,
	      "the next mount is rw");
	g_sys.close_fd(g_sys.ctx, at2.a);

	/* chkdsk-flag: only the informational flag is left, still clean (N-04) */
	reset_image();
	run("rm -rf '%s'", g_jdir);
	ntfsmac_journal_close(j);
	CHECK(ntfsmac_journal_open(g_jdir, &j) == NTFSMAC_JOURNAL_OK, "the journal opens again");
	priv_init(j);
	CHECK(make_unclean(j, &at), "a second unclean session");
	CHECK(run("'%s' '%s' --clear DIRTY --set MODIFIED_BY_CHKDSK >/dev/null", g_setflags, g_image) == 0, "chkdsk-flag image");
	CHECK(run("'%s' '%s' --bump-lsn >/dev/null", g_logmark, g_image) == 0, "logfile_mark --bump-lsn");
	CHECK(priv_jc(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_OK && strstr(reply, "\"warning\":\"none\"") != NULL,
	      "accepted on chkdsk-flag with an entry and a moved log: %s", reply);
	ntfsmac_journal_close(j);
}

static void case_repair(void)
{
	ntfsmac_journal *j;
	attach at;
	char reply[512], ser[16], label[80], sha[128];
	ntfsmac_probe_verdict v;
	long inside, outside;
	ntfsmac_journal_entry e;

	printf("== repair: repair --clear-dirty on real images (I-DMN-21, module level)\n");
	reset_image();
	run("rm -rf '%s' && cp '%s' '%s.pre'", g_jdir, g_image, g_image);
	spawn_cfg(NULL);
	CHECK(ntfsmac_journal_open(g_jdir, &j) == NTFSMAC_JOURNAL_OK, "the journal opens");
	priv_init(j);
	CHECK(probe_now(&v), "probe");
	serial8(&v, ser);
	snprintf(label, sizeof(label), "%s", v.label);

	/* refusals: clean, unclean log, an extra flag: the image hash never changes */
	CHECK(priv_rp(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED && strstr(reply, "probe_not_dirty_flag") != NULL,
	      "refused on a clean volume");
	run("'%s' '%s' --set DIRTY,REPAIR_OBJECT_ID >/dev/null", g_setflags, g_image);
	sha_of(g_image, sha, sizeof(sha));
	/* the probe reports FLAG_PENDING_WORK next to FLAG_DIRTY: not exactly the dirty-flag state */
	CHECK(priv_rp(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED &&
		      (strstr(reply, "probe_not_dirty_flag") != NULL || strstr(reply, "\"flags\"") != NULL),
	      "refused with a flag besides the informational ones (%s)", reply);
	CHECK(same_sha(g_image, sha), "the image is unchanged after the refusal");
	reset_image();
	run("'%s' '%s' --unclean >/dev/null && '%s' '%s' --set DIRTY >/dev/null", g_logmark, g_image, g_setflags, g_image);
	sha_of(g_image, sha, sizeof(sha));
	CHECK(priv_rp(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED, "refused with an unclean log (%s)", reply);
	CHECK(same_sha(g_image, sha), "the image is unchanged after the refusal");

	/* dirty-flag: accepted; wrong confirmation first, then right, then mounted */
	reset_image();
	run("'%s' '%s' --set DIRTY >/dev/null", g_setflags, g_image);
	run("cp '%s' '%s.pre'", g_image, g_image);
	sha_of(g_image, sha, sizeof(sha));
	CHECK(priv_rp("nope", ser, reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED && strstr(reply, "confirmation") != NULL,
	      "refused with a wrong label");
	g_vol_mounted = true;
	CHECK(priv_rp(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED && strstr(reply, "mounted") != NULL,
	      "refused while mounted");
	g_vol_mounted = false;
	CHECK(same_sha(g_image, sha), "the image is unchanged after the refusals");
	CHECK(strcmp(probe_state_now(), "dirty_flag") == 0, "the volume probes dirty_flag");
	CHECK(priv_rp(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_OK && strstr(reply, "repaired") != NULL,
	      "accepted on dirty-flag (%s)", reply);
	CHECK(strcmp(probe_state_now(), "clean") == 0, "the flag is clear afterwards (probe: clean)");
	outside = diff_outside(g_image, g_image, &inside); /* self test of the helper: no difference */
	CHECK(outside == 0 && inside == 0, "diff helper sanity");
	{
		char before[320];

		snprintf(before, sizeof(before), "%s.pre", g_image);
		outside = diff_outside(before, g_image, &inside);
		CHECK(outside == 0 && inside > 0, "only the $Volume record of $MFT and $MFTMirr changed (%ld bytes inside, %ld outside)",
		      inside, outside);
	}

	/* dirty-chkdsk-flag keeps the informational bit */
	reset_image();
	run("'%s' '%s' --set DIRTY,MODIFIED_BY_CHKDSK >/dev/null", g_setflags, g_image);
	CHECK(priv_rp(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_OK, "accepted on dirty-chkdsk-flag (%s)", reply);
	CHECK(probe_now(&v) && (v.volume_flags & NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_MODIFIED_BY_CHKDSK)) != 0 &&
		      (v.volume_flags & NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_DIRTY)) == 0,
	      "DIRTY is clear and the 0x8000 bit is kept");

	/* with a journal entry: removed, and the extra warning is in the reply */
	reset_image();
	run("rm -rf '%s'", g_jdir);
	ntfsmac_journal_close(j);
	CHECK(ntfsmac_journal_open(g_jdir, &j) == NTFSMAC_JOURNAL_OK, "the journal opens again");
	priv_init(j);
	CHECK(make_unclean(j, &at), "an unclean session (entry, flag set)");
	CHECK(entry_get(j, at.v.serial, &e), "the entry exists");
	CHECK(priv_rp(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_OK && strstr(reply, "\"journal_entry\":\"removed\"") != NULL &&
		      strstr(reply, "\"warning\":\"journal_entry\"") != NULL,
	      "accepted, the entry is removed and the warning is there (%s)", reply);
	CHECK(!entry_get(j, at.v.serial, &e), "the entry is gone");
	ntfsmac_journal_close(j);
	run("rm -f '%s.pre'", g_image);
}

int main(int argc, char **argv)
{
	/* The worker's start runs this program once more as `--exec-as` inside the user's session. */
	if (argc >= 2 && strcmp(argv[1], NTFSMAC_EXEC_AS_FLAG) == 0)
		return ntfsmac_spawn_exec_as(argc, argv);
	char path[320];
	const char *only;

	if (argc < 11) {
		fprintf(stderr,
			"usage: journal_vm WORKDIR UID GID SBXDIR PROBE VOLMARK WORKER STUBDIR SETFLAGS LOGMARK [CASE]\n");
		return 2;
	}
	g_work = argv[1];
	g_uid = (uid_t)strtoul(argv[2], NULL, 10);
	g_gid = (gid_t)strtoul(argv[3], NULL, 10);
	g_sbx = argv[4];
	g_probe = argv[5];
	g_volmark = argv[6];
	g_worker = argv[7];
	g_stubdir = argv[8];
	g_setflags = argv[9];
	g_logmark = argv[10];
	only = argc > 11 ? argv[11] : NULL;
	if (geteuid() != 0 || g_uid == 0) {
		fprintf(stderr, "journal_vm: run as root, with a console user that is not root\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	snprintf(g_base, sizeof(g_base), "%s/base.img", g_work);
	snprintf(g_image, sizeof(g_image), "%s/ntfs.img", g_work);
	snprintf(g_image2, sizeof(g_image2), "%s/img2.img", g_work);
	snprintf(g_jdir, sizeof(g_jdir), "%s/journal", g_work);
	snprintf(path, sizeof(path), "%s/worker.log", g_work);
	g_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
	sys_init();
	spawn_cfg(NULL);

	if (only == NULL || !strcmp(only, "rw"))
		case_rw();
	if (only == NULL || !strcmp(only, "unclean"))
		case_unclean();
	if (only == NULL || !strcmp(only, "markfail"))
		case_markfail();
	if (only == NULL || !strcmp(only, "bind"))
		case_bind();
	if (only == NULL || !strcmp(only, "evidence"))
		case_evidence();
	if (only == NULL || !strcmp(only, "clear"))
		case_clear();
	if (only == NULL || !strcmp(only, "repair"))
		case_repair();
	printf("== result: %d failure(s)\n", g_fails);
	return g_fails == 0 ? 0 : 1;
}
