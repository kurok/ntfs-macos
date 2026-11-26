/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Session lifecycle of `ntfsmacd` (T-M4-14): journal entry, binding of the probed volume to the
 * opened device (ADR-020), the marker ordering of docs/ARCHITECTURE.md section 3.6, the end of a
 * session (section 4.5 step 3) and the decision step with the evidence rule (section 3.5).
 *
 * The code decides; the system is reached only through ntfsmac_session_sys, so the ordering
 * and every failure branch are unit tested with fakes (tests/unit/daemon/session_test.c) and the
 * real calls are exercised in a disposable guest (tests/integration/daemon/journal_vm.sh).
 *
 * The rules this file keeps, in the order they matter:
 *   - no writable descriptor exists before the journal entry is on disk, and none is kept
 *     unless it was bound to the probed volume;
 *   - the worker is started only after the journal says marker=set;
 *   - every failure on that path ends in a ro session on a freshly opened and freshly bound
 *     read-only descriptor, with the entry kept (marker=failed);
 *   - an entry is removed at the end of a session only on the full positive proof.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ctl_priv.h"
#include "internal.h"
#include "sanitize.h"

#if defined(__APPLE__)
#include <sys/disk.h>
#endif

/* ------------------------------------------------------------------ */
/* Pure parts                                                          */
/* ------------------------------------------------------------------ */

ntfsmac_policy_mode ntfsmac_session_policy_mode(const ntfsmac_session_config *cfg,
						bool automatic, bool request_ro)
{
	if (automatic && cfg != NULL && cfg->automount_ro)
		return NTFSMAC_POLICY_MODE_RO;
	return request_ro ? NTFSMAC_POLICY_MODE_RO : NTFSMAC_POLICY_MODE_AUTO;
}

const char *ntfsmac_bind_result_name(ntfsmac_bind_result r)
{
	switch (r) {
	case NTFSMAC_BIND_OK:
		return "ok";
	case NTFSMAC_BIND_APPLE_MOUNTED:
		return "apple_mounted";
	case NTFSMAC_BIND_OTHER_MOUNTED:
		return "other_mounted";
	case NTFSMAC_BIND_DISK_CHANGED:
		return "disk_changed";
	case NTFSMAC_BIND_NO_ANSWER:
		return "no_answer";
	}
	return "?";
}

/*
 * ADR-020 (5), section 4.1 step 6: the Disk Arbitration description of the disk is that of the
 * object the probe ran on (same BSD name and DAMediaPath) and no file system has it mounted.
 * Apple's driver mounts NTFS as kind "ntfs"; that is the one mount that leads to ro.
 */
ntfsmac_bind_result ntfsmac_bind_check_disk(const char *probed_bsd, const char *probed_media_path,
					    const ntfsmac_disk_info *now,
					    ntfsmac_dmn_status describe_status)
{
	if (describe_status != NTFSMAC_DMN_OK || now == NULL || probed_bsd == NULL ||
	    probed_media_path == NULL)
		return NTFSMAC_BIND_NO_ANSWER;
	if (strncmp(now->bsd_name, probed_bsd, sizeof now->bsd_name) != 0 ||
	    strncmp(now->media_path, probed_media_path, sizeof now->media_path) != 0)
		return NTFSMAC_BIND_DISK_CHANGED;
	if (now->has_volume_path)
		return strncmp(now->volume_kind, "ntfs", sizeof now->volume_kind) == 0
			       ? NTFSMAC_BIND_APPLE_MOUNTED
			       : NTFSMAC_BIND_OTHER_MOUNTED;
	return NTFSMAC_BIND_OK;
}

bool ntfsmac_bind_same_device(const ntfsmac_dev_id *a, const ntfsmac_dev_id *b)
{
	return a != NULL && b != NULL && a->a == b->a && a->b == b->b;
}

/* ------------------------------------------------------------------ */
/* The real system facts                                               */
/* ------------------------------------------------------------------ */

static int real_open_device(void *ctx, const char *bsd, bool writable)
{
	char path[64];

	(void)ctx;
	if (!ntfsmac_ctl_bsd_valid(bsd)) /* "diskN" or "diskNsM"; never /dev/rdisk*, never a path */
		return -1;
	snprintf(path, sizeof path, "/dev/%s", bsd);
	return open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
}

static void real_close_fd(void *ctx, int fd)
{
	(void)ctx;
	if (fd >= 0)
		(void)close(fd);
}

/* A device node is known by st_rdev; an image file (tests) by device and inode. */
static int real_dev_identity(void *ctx, int fd, ntfsmac_dev_id *out)
{
	struct stat st;

	(void)ctx;
	if (fstat(fd, &st) != 0)
		return -1;
	if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
		out->a = (uint64_t)st.st_rdev;
		out->b = 1u;
	} else if (S_ISREG(st.st_mode)) {
		out->a = (uint64_t)st.st_dev;
		out->b = (uint64_t)st.st_ino << 1;
	} else {
		return -1;
	}
	return 0;
}

static int real_read_block0(void *ctx, int fd, uint8_t *buf)
{
	size_t have = 0;

	(void)ctx;
	while (have < NTFSMAC_BLOCK0_BYTES) {
		ssize_t n = pread(fd, buf + have, NTFSMAC_BLOCK0_BYTES - have, (off_t)have);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		have += (size_t)n;
	}
	return 0;
}

/*
 * Clear what a child could have set on the shared open file: O_NONBLOCK, O_APPEND and O_ASYNC.
 * A disk device node answers fcntl(F_SETFL) with ENOTTY even when nothing changes (found in the
 * first VM run of T-M4-08), so F_SETFL is called only when a flag is set.
 */
int ntfsmac_fd_reset_flags(int fd)
{
	int fl = fcntl(fd, F_GETFL);

	if (fl < 0)
		return -1;
	if ((fl & (O_NONBLOCK | O_APPEND | O_ASYNC)) == 0)
		return 0;
	return fcntl(fd, F_SETFL, fl & ~(O_NONBLOCK | O_APPEND | O_ASYNC));
}

static int real_reset_flags(void *ctx, int fd)
{
	(void)ctx;
	return ntfsmac_fd_reset_flags(fd);
}

static ntfsmac_dmn_status real_describe(void *ctx, const char *bsd, ntfsmac_disk_info *out)
{
	ntfsmac_session_sys_real *r = ctx;

	if (r->da == NULL)
		return NTFSMAC_DMN_E_NOTIMPL;
	return ntfsmac_da_describe(r->da, bsd, out);
}

static ntfsmac_dmn_status real_volmark(void *ctx, const ntfsmac_volmark_run *run,
				       ntfsmac_volmark_op op, int *exit_code)
{
	(void)ctx;
	return ntfsmac_spawn_volmark_op(run, op, exit_code);
}

/* Section 4.5 step 3: fsync (failure: not clean), then the drive flush (failure: a warning). */
static ntfsmac_dmn_status real_sync_device(void *ctx, int fd, bool *flush_ok)
{
	struct stat st;

	(void)ctx;
	*flush_ok = true;
	if (fsync(fd) != 0)
		return NTFSMAC_DMN_E_IO;
#if defined(__APPLE__)
	if (fstat(fd, &st) == 0 && (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode))) {
		dk_synchronize_t dk;

		memset(&dk, 0, sizeof dk);
		if (ioctl(fd, DKIOCSYNCHRONIZE, &dk) != 0)
			*flush_ok = false;
	} else if (fcntl(fd, F_FULLFSYNC) == -1) {
		*flush_ok = false; /* an image file in tests */
	}
#else
	(void)st;
#endif
	return NTFSMAC_DMN_OK;
}

static int64_t real_now(void *ctx)
{
	(void)ctx;
	return (int64_t)time(NULL);
}

static uint64_t real_new_session_id(void *ctx)
{
	uint64_t id = 0;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

	(void)ctx;
	if (fd >= 0) {
		if (read(fd, &id, sizeof id) != (ssize_t)sizeof id)
			id = 0;
		close(fd);
	}
	return id != 0 ? id : (uint64_t)time(NULL) | 1u;
}

void ntfsmac_session_sys_real_init(ntfsmac_session_sys_real *r, ntfsmac_da *da)
{
	memset(r, 0, sizeof *r);
	r->da = da;
	r->sys.ctx = r;
	r->sys.open_device = real_open_device;
	r->sys.close_fd = real_close_fd;
	r->sys.dev_identity = real_dev_identity;
	r->sys.read_block0 = real_read_block0;
	r->sys.reset_flags = real_reset_flags;
	r->sys.describe = real_describe;
	r->sys.volmark = real_volmark;
	r->sys.sync_device = real_sync_device;
	r->sys.now = real_now;
	r->sys.new_session_id = real_new_session_id;
}

/* ------------------------------------------------------------------ */
/* Compare and remove                                                  */
/* ------------------------------------------------------------------ */

/*
 * Creating an entry (session_begin) and removing one after a gate or a slow step happen on
 * different threads. The lock makes "is this still the entry I checked" and the removal one
 * step against a new session of the same serial: a new entry always survives.
 */
static pthread_mutex_t g_entry_mu = PTHREAD_MUTEX_INITIALIZER;

ntfsmac_journal_status ntfsmac_journal_remove_if(ntfsmac_journal *j,
						 const ntfsmac_journal_entry *expected,
						 ntfsmac_journal_removal path, int64_t now)
{
	ntfsmac_journal_entry cur;
	ntfsmac_journal_status js;
	uint64_t ts;

	if (j == NULL || expected == NULL)
		return NTFSMAC_JOURNAL_ERR_INVALID;
	/* the key of the entry the caller looked at: format 1 has no identity (T-M7-13) */
	ts = expected->legacy ? 0 : expected->total_sectors;
	pthread_mutex_lock(&g_entry_mu);
	js = ntfsmac_journal_get(j, expected->serial, ts, &cur);
	if (js == NTFSMAC_JOURNAL_OK) {
		/* the entry the caller looked at: same session, same creation time, same format */
		if (cur.session_id != expected->session_id || cur.created != expected->created ||
		    cur.legacy != expected->legacy)
			js = NTFSMAC_JOURNAL_ERR_PATH_CONDITION;
		else
			js = ntfsmac_journal_remove(j, expected->serial, ts, path, now);
	} else if (js == NTFSMAC_JOURNAL_ERR_CORRUPT && expected->session_id == 0 &&
		   expected->created == 0) {
		/* the stand-in of a corrupt entry: the file that the same lookup found */
		js = ntfsmac_journal_remove(j, expected->serial, ts, path, now);
	} else if (js == NTFSMAC_JOURNAL_ERR_CORRUPT) {
		js = NTFSMAC_JOURNAL_ERR_PATH_CONDITION;
	}
	pthread_mutex_unlock(&g_entry_mu);
	return js;
}

/* ------------------------------------------------------------------ */
/* Decision step (section 4.1 step 5, section 3.5 rule 2)              */
/* ------------------------------------------------------------------ */

const char *ntfsmac_session_serial_token(const ntfsmac_session_decision *d)
{
	if (d == NULL || !d->serial_live)
		return NULL;
	switch (d->policy.note) {
	case NTFSMAC_POLICY_NOTE_EXTERNAL_CHECK:
	case NTFSMAC_POLICY_NOTE_UNCLEAN_SESSION:
	case NTFSMAC_POLICY_NOTE_DIRTY_FLAG_JOURNAL:
		return "same_serial"; /* these notes come from the entry, and the entry is another disk's */
	default:
		return NULL; /* hibernation, dirty flag, unclean log, names not NFC ...: the volume's own reason */
	}
}

ntfsmac_dmn_status ntfsmac_session_decide(const ntfsmac_session_config *cfg, ntfsmac_journal *j,
					  const ntfsmac_probe_verdict *v, bool automatic,
					  bool request_ro, int64_t now,
					  bool (*session_live)(void *, uint64_t), void *live_ctx,
					  ntfsmac_session_decision *out)
{
	ntfsmac_journal_entry e, synth;
	const ntfsmac_journal_entry *ep = NULL;
	ntfsmac_policy_input in;
	ntfsmac_journal_status js;
	bool live;

	if (cfg == NULL || j == NULL || v == NULL || out == NULL)
		return NTFSMAC_DMN_E_INVAL;
	memset(out, 0, sizeof *out);

	/*
	 * The entry of this volume: serial and total_sectors (T-M7-13). An entry of another volume
	 * with the same serial is not looked at and not touched; a format-1 entry of the serial has
	 * no identity and applies, as before.
	 */
	js = ntfsmac_journal_get(j, v->serial, v->total_sectors, &e);
	if (js == NTFSMAC_JOURNAL_OK) {
		ep = &e;
	} else if (js != NTFSMAC_JOURNAL_ERR_NOT_FOUND) {
		/*
		 * Corrupt or unreadable: it is never repaired silently and blocks rules 1 and 2
		 * (section 3.5). A stand-in with marker `failed` blocks the evidence rule, and
		 * any entry makes the policy answer ro.
		 */
		memset(&synth, 0, sizeof synth);
		synth.serial = v->serial;
		synth.total_sectors = v->total_sectors; /* it stands in for this volume's entry */
		synth.marker = NTFSMAC_JOURNAL_MARKER_FAILED;
		synth.state = NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
		ep = &synth;
		out->entry_corrupt = true;
	}

	memset(&in, 0, sizeof in);
	in.verdict = v;
	in.journal = ep;
	in.mode = ntfsmac_session_policy_mode(cfg, automatic, request_ro);
	in.evidence_rule_enabled = cfg->evidence_rule_enabled;
	out->policy = ntfsmac_policy_decide(&in);
	/* An entry of a live session is no leftover of an unclean session: another attached disk has this
	 * serial (a clone). session_live is asked once. */
	live = ep != NULL && !out->entry_corrupt && ep->session_id != 0 && session_live != NULL &&
	       session_live(live_ctx, ep->session_id);
	out->serial_live = live;

	if (out->policy.journal_action == NTFSMAC_POLICY_JOURNAL_REMOVE && ep != NULL &&
	    !out->entry_corrupt) {
		/*
		 * Rule 2. It takes effect on the next decision, never on this attach, and never on the
		 * entry of a live session (a clone of a volume that another disk has mounted rw).
		 */
		if (!live)
			out->evidence_removed =
				ntfsmac_journal_remove_if(j, ep, NTFSMAC_JOURNAL_REMOVE_EXTERNAL_CHECK,
							  now) == NTFSMAC_JOURNAL_OK;
	}
	return NTFSMAC_DMN_OK;
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

struct ntfsmac_session {
	const ntfsmac_session_sys *sys;
	ntfsmac_session_config cfg;
	ntfsmac_journal *j;
	ntfsmac_vol_mode mode;
	int fd; /* -1 once closed */
	bool has_entry;
	ntfsmac_journal_entry entry;
};

static void sess_close(ntfsmac_session *s)
{
	if (s->fd >= 0) {
		s->sys->close_fd(s->sys->ctx, s->fd);
		s->fd = -1;
	}
}

/* Change marker and state of the entry (updated stamped); the entry is kept only if it was stored. */
static ntfsmac_journal_status entry_change(ntfsmac_session *s, ntfsmac_journal_marker marker,
					   ntfsmac_journal_state state)
{
	ntfsmac_journal_entry e = s->entry;
	ntfsmac_journal_status js;

	e.marker = marker;
	e.state = state;
	e.updated = s->sys->now(s->sys->ctx);
	js = ntfsmac_journal_update(s->j, &e);
	if (js == NTFSMAC_JOURNAL_OK)
		s->entry = e;
	return js;
}

/* section 3.6 step 2: record that the marker step did not complete. Best effort; the entry stays. */
static void entry_mark_failed(ntfsmac_session *s)
{
	if (s->has_entry && s->entry.marker == NTFSMAC_JOURNAL_MARKER_PENDING)
		(void)entry_change(s, NTFSMAC_JOURNAL_MARKER_FAILED, s->entry.state);
}

static bool config_ok_sys(const ntfsmac_session_sys *sys)
{
	return sys != NULL && sys->open_device != NULL && sys->close_fd != NULL &&
	       sys->dev_identity != NULL && sys->read_block0 != NULL && sys->reset_flags != NULL &&
	       sys->describe != NULL && sys->volmark != NULL && sys->sync_device != NULL &&
	       sys->now != NULL && sys->new_session_id != NULL;
}

/* Disk Arbitration binding for a new descriptor. true: the volume may go on (ro). */
static bool bind_disk(const ntfsmac_session_sys *sys, const ntfsmac_session_begin_in *in,
		      ntfsmac_bind_result *result)
{
	ntfsmac_disk_info di;
	ntfsmac_dmn_status ds;

	memset(&di, 0, sizeof di);
	ds = sys->describe(sys->ctx, in->bsd, &di);
	*result = ntfsmac_bind_check_disk(in->bsd, in->media_path, &di, ds);
	return *result == NTFSMAC_BIND_OK || *result == NTFSMAC_BIND_APPLE_MOUNTED;
}

static ntfsmac_session *session_new(const ntfsmac_session_sys *sys,
				    const ntfsmac_session_config *cfg, ntfsmac_journal *j)
{
	ntfsmac_session *s = calloc(1, sizeof *s);

	if (s == NULL)
		return NULL;
	s->sys = sys;
	s->cfg = *cfg;
	s->j = j;
	s->fd = -1;
	s->mode = NTFSMAC_VMODE_NONE;
	return s;
}

static ntfsmac_dmn_status denied(ntfsmac_session *s, ntfsmac_session_start *res)
{
	if (s != NULL) {
		sess_close(s);
		ntfsmac_session_free(s);
	}
	res->mode = NTFSMAC_VMODE_NONE;
	res->failed = NTFSMAC_VREASON_BINDING;
	return NTFSMAC_DMN_E_DENIED;
}

/*
 * A read-only session on descriptor A (the probe's own): its flags are reset after the probe
 * exited and the first 4096 bytes are read again; another hash is `Failed` (step 6, "For ro").
 */
static ntfsmac_dmn_status finish_ro_on_a(ntfsmac_session *s, const ntfsmac_session_begin_in *in,
					 ntfsmac_session_reason reason, ntfsmac_session **out,
					 ntfsmac_session_start *res)
{
	uint8_t now[NTFSMAC_BLOCK0_BYTES];

	if (s->sys->reset_flags(s->sys->ctx, s->fd) != 0 ||
	    s->sys->read_block0(s->sys->ctx, s->fd, now) != 0 ||
	    memcmp(now, in->block0, sizeof now) != 0)
		return denied(s, res);
	s->mode = NTFSMAC_VMODE_RO;
	*out = s;
	res->mode = NTFSMAC_VMODE_RO;
	res->reason = reason;
	res->failed = NTFSMAC_VREASON_NONE;
	res->session_id = s->has_entry ? s->entry.session_id : 0;
	return NTFSMAC_DMN_OK;
}

/*
 * Section 3.6 ordering step 2: every descriptor closed, a fresh O_RDONLY descriptor opened and
 * bound again (same device as A was, same first 4096 bytes, Disk Arbitration), the session
 * continues ro. Nothing can write in this attach.
 */
static ntfsmac_dmn_status ro_fallback(ntfsmac_session *s, const ntfsmac_session_begin_in *in,
				      const ntfsmac_dev_id *id_a, ntfsmac_session_reason reason,
				      ntfsmac_session **out, ntfsmac_session_start *res)
{
	const ntfsmac_session_sys *sys = s->sys;
	ntfsmac_dev_id id;
	uint8_t now[NTFSMAC_BLOCK0_BYTES];
	ntfsmac_bind_result br;
	int fd;

	sess_close(s); /* B, if one is open */
	fd = sys->open_device(sys->ctx, in->bsd, false);
	if (fd < 0)
		return denied(s, res);
	s->fd = fd;
	if (sys->dev_identity(sys->ctx, fd, &id) != 0 || !ntfsmac_bind_same_device(&id, id_a) ||
	    sys->read_block0(sys->ctx, fd, now) != 0 || memcmp(now, in->block0, sizeof now) != 0 ||
	    !bind_disk(sys, in, &br))
		return denied(s, res);
	s->mode = NTFSMAC_VMODE_RO;
	*out = s;
	res->mode = NTFSMAC_VMODE_RO;
	res->reason = reason;
	res->failed = NTFSMAC_VREASON_NONE;
	res->session_id = s->has_entry ? s->entry.session_id : 0;
	return NTFSMAC_DMN_OK;
}

ntfsmac_dmn_status ntfsmac_session_begin(const ntfsmac_session_sys *sys,
					 const ntfsmac_session_config *cfg, ntfsmac_journal *j,
					 const ntfsmac_session_begin_in *in,
					 ntfsmac_session **out, ntfsmac_session_start *res)
{
	const ntfsmac_probe_verdict *v;
	ntfsmac_session *s;
	ntfsmac_dev_id id_a, id_b;
	ntfsmac_bind_result br;
	uint8_t now[NTFSMAC_BLOCK0_BYTES];
	ntfsmac_journal_entry e;
	ntfsmac_volmark_run run;
	ntfsmac_dmn_status st;
	int exit_code = -3, fd;
	bool rw;
	ntfsmac_session_reason apple = NTFSMAC_SREASON_NONE;
	ntfsmac_journal_status js_create;

	if (out != NULL)
		*out = NULL;
	if (res != NULL)
		memset(res, 0, sizeof *res);
	if (!config_ok_sys(sys) || cfg == NULL || j == NULL || in == NULL || in->verdict == NULL ||
	    out == NULL || res == NULL || in->bsd == NULL || !ntfsmac_ctl_bsd_valid(in->bsd) ||
	    (in->decision != NTFSMAC_POLICY_RW && in->decision != NTFSMAC_POLICY_RO)) {
		/* The descriptor is the session's to close on every return, also with a broken sys. */
		if (in != NULL && in->probe_fd >= 0) {
			if (config_ok_sys(sys))
				sys->close_fd(sys->ctx, in->probe_fd);
			else
				(void)close(in->probe_fd);
		}
		return NTFSMAC_DMN_E_INVAL;
	}
	v = in->verdict;
	s = session_new(sys, cfg, j);
	if (s == NULL) {
		sys->close_fd(sys->ctx, in->probe_fd);
		return NTFSMAC_DMN_E_IO;
	}
	s->fd = in->probe_fd; /* from here the session owns A */
	res->mode = NTFSMAC_VMODE_NONE;

	/* Binding, part 1: the object is the one the probe ran on, and nobody has it mounted. */
	if (sys->dev_identity(sys->ctx, s->fd, &id_a) != 0 || !bind_disk(sys, in, &br))
		return denied(s, res);
	if (br == NTFSMAC_BIND_APPLE_MOUNTED)
		apple = NTFSMAC_SREASON_OTHER_DRIVER;

	rw = in->decision == NTFSMAC_POLICY_RW && br == NTFSMAC_BIND_OK;
	if (!rw)
		return finish_ro_on_a(s, in, apple, out, res); /* ro as decided, or Apple's driver */

	/* ---- rw: the ordering of section 3.6 ---- */

	/* 1. The journal entry, before any writable descriptor exists. */
	memset(&e, 0, sizeof e);
	e.serial = v->serial;
	if (ntfsmac_label_sanitize(v->label, v->label_len, v->serial, e.label,
				   sizeof e.label) == 0 ||
	    !v->has_logfile_lsn) {
		/* A clean verdict has a log sequence number (section 3.1); without it no rw. */
		return ro_fallback(s, in, &id_a, NTFSMAC_SREASON_JOURNAL, out, res);
	}
	e.total_sectors = v->total_sectors;
	e.session_id = sys->new_session_id(sys->ctx);
	e.created = e.updated = sys->now(sys->ctx);
	e.logfile_lsn_at_start = v->logfile_lsn;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING; /* a rw decision always runs volmark (section 3.6) */
#if defined(NTFSMAC_TEST_SEAMS)
	if (cfg->marker_disabled)
		e.marker = NTFSMAC_JOURNAL_MARKER_NONE;
#endif
	e.state = NTFSMAC_JOURNAL_STATE_OPEN;
	pthread_mutex_lock(&g_entry_mu);
	js_create = e.session_id == 0 ? NTFSMAC_JOURNAL_ERR_INVALID : ntfsmac_journal_create(j, &e);
	pthread_mutex_unlock(&g_entry_mu);
	if (js_create != NTFSMAC_JOURNAL_OK) {
		/* An entry that already exists is not ours to touch: the policy said rw only without one. */
		return ro_fallback(s, in, &id_a, NTFSMAC_SREASON_JOURNAL, out, res);
	}
	s->has_entry = true;
	s->entry = e;

	/* 2. B, opened while A is open; same device, same first 4096 bytes; only then close A. */
	fd = sys->open_device(sys->ctx, in->bsd, true);
	if (fd < 0) {
		entry_mark_failed(s);
		return ro_fallback(s, in, &id_a, NTFSMAC_SREASON_BINDING, out, res);
	}
	/* A stays in s->fd until B is bound, so that ro_fallback() closes it with the rest. */
	{
		int a_fd = s->fd;

		s->fd = fd;
		if (sys->dev_identity(sys->ctx, fd, &id_b) != 0 ||
		    !ntfsmac_bind_same_device(&id_a, &id_b) ||
		    sys->read_block0(sys->ctx, fd, now) != 0 ||
		    memcmp(now, in->block0, sizeof now) != 0) {
			sys->close_fd(sys->ctx, a_fd);
			entry_mark_failed(s);
			return ro_fallback(s, in, &id_a, NTFSMAC_SREASON_BINDING, out, res);
		}
		sys->close_fd(sys->ctx, a_fd);
	}

	/* 3. volmark set, then the first 4096 bytes again, then marker=set; then the worker. */
	if (s->entry.marker == NTFSMAC_JOURNAL_MARKER_PENDING) {
		unsigned t = cfg->volmark_timeout_s != 0 ? cfg->volmark_timeout_s : 20u;

		memset(&run, 0, sizeof run);
		run.device_fd = s->fd;
		run.serial = v->serial;
		run.total_sectors = v->total_sectors;
		run.timeout_s = t;
		st = sys->volmark(sys->ctx, &run, NTFSMAC_VOLMARK_SET, &exit_code);
		if (st == NTFSMAC_DMN_E_BUSY && exit_code == -4) {
			/* volmark could not be reaped after SIGKILL: it may still hold the device. */
			entry_mark_failed(s);
			sess_close(s);
			ntfsmac_session_free(s);
			res->mode = NTFSMAC_VMODE_NONE;
			res->failed = NTFSMAC_VREASON_SPAWN;
			return NTFSMAC_DMN_E_DENIED;
		}
		if (st != NTFSMAC_DMN_OK || exit_code != 0) {
			/* -2: timed out after `w`, a write may have been cut: the residual text */
			ntfsmac_session_reason why = (st == NTFSMAC_DMN_E_TIMEOUT && exit_code == -2)
							     ? NTFSMAC_SREASON_MARKER_INTERRUPTED
							     : NTFSMAC_SREASON_MARKER_FAILED;

			entry_mark_failed(s);
			return ro_fallback(s, in, &id_a, why, out, res);
		}
		if (sys->read_block0(sys->ctx, s->fd, now) != 0 ||
		    memcmp(now, in->block0, sizeof now) != 0) {
			/* volmark touched sector 0, or the device changed: the flag may be on the disk */
			entry_mark_failed(s);
			return ro_fallback(s, in, &id_a, NTFSMAC_SREASON_BINDING_AFTER_MARK, out, res);
		}
		if (entry_change(s, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN) !=
		    NTFSMAC_JOURNAL_OK) {
			/* The flag is on the disk but the journal cannot say so: no worker write. */
			entry_mark_failed(s);
			return ro_fallback(s, in, &id_a, NTFSMAC_SREASON_MARKER_FAILED, out, res);
		}
	}

	s->mode = NTFSMAC_VMODE_RW;
	*out = s;
	res->mode = NTFSMAC_VMODE_RW;
	res->reason = NTFSMAC_SREASON_NONE;
	res->failed = NTFSMAC_VREASON_NONE;
	res->session_id = s->entry.session_id;
	return NTFSMAC_DMN_OK;
}

int ntfsmac_session_device_fd(const ntfsmac_session *s)
{
	return s != NULL ? s->fd : -1;
}

ntfsmac_vol_mode ntfsmac_session_mode(const ntfsmac_session *s)
{
	return s != NULL ? s->mode : NTFSMAC_VMODE_NONE;
}

uint64_t ntfsmac_session_id(const ntfsmac_session *s)
{
	return s != NULL && s->has_entry ? s->entry.session_id : 0;
}

bool ntfsmac_session_owns_entry(const ntfsmac_session *s)
{
	return s != NULL && s->has_entry;
}

ntfsmac_dmn_status ntfsmac_session_note_write_error(ntfsmac_session *s)
{
	if (s == NULL)
		return NTFSMAC_DMN_E_INVAL;
	if (!s->has_entry || s->entry.state == NTFSMAC_JOURNAL_STATE_WRITE_ERROR)
		return NTFSMAC_DMN_OK;
	return entry_change(s, s->entry.marker, NTFSMAC_JOURNAL_STATE_WRITE_ERROR) ==
			       NTFSMAC_JOURNAL_OK
		       ? NTFSMAC_DMN_OK
		       : NTFSMAC_DMN_E_IO;
}

ntfsmac_dmn_status ntfsmac_session_note_mount(ntfsmac_session *s, uint32_t owner, uint64_t fsid,
					      const char *from)
{
	ntfsmac_journal_entry e;
	size_t n;

	if (s == NULL || from == NULL)
		return NTFSMAC_DMN_E_INVAL;
	n = strnlen(from, sizeof e.mount_from);
	if (n == 0 || n >= sizeof e.mount_from)
		return NTFSMAC_DMN_E_INVAL;
	if (!s->has_entry || s->entry.has_mount || s->entry.legacy)
		return NTFSMAC_DMN_OK;
	e = s->entry;
	e.has_mount = true;
	e.mount_owner = owner;
	e.mount_fsid = fsid;
	memcpy(e.mount_from, from, n);
	e.mount_from[n] = '\0';
	e.updated = s->sys->now(s->sys->ctx);
	if (ntfsmac_journal_update(s->j, &e) != NTFSMAC_JOURNAL_OK)
		return NTFSMAC_DMN_E_IO; /* unprintable bytes come back as INVALID: also no record */
	s->entry = e;
	return NTFSMAC_DMN_OK;
}

ntfsmac_dmn_status ntfsmac_session_end(ntfsmac_session *s, const ntfsmac_errwatch *w,
				       bool unmount_seen_first, int worker_exit_status, bool force_unclean,
				       ntfsmac_session_end_result *res)
{
	ntfsmac_volmark_run run;
	bool flush_ok = true;
	int exit_code = -3;
	ntfsmac_dmn_status st;

	if (s == NULL || res == NULL)
		return NTFSMAC_DMN_E_INVAL;
	memset(res, 0, sizeof *res);
	if (s->fd < 0)
		return NTFSMAC_DMN_E_STATE; /* ended already */

	/* An error record or a broken channel is recorded whatever else happens. */
	if (w != NULL &&
	    (w->flags & (NTFSMAC_EW_SAW_E | NTFSMAC_EW_PROTOCOL_ERROR)) != 0)
		(void)ntfsmac_session_note_write_error(s);

	if (s->mode != NTFSMAC_VMODE_RW || !s->has_entry) {
		res->report = NTFSMAC_SREPORT_RO_CLOSED;
		sess_close(s);
		return NTFSMAC_DMN_OK;
	}

	/* Rule 1 of section 3.5, one condition after the other; the first that fails keeps the entry. */
	if (force_unclean) /* best effort; the report does not depend on the journal write */
		(void)ntfsmac_session_note_write_error(s);
	if (force_unclean || w == NULL || !ntfsmac_errwatch_clean_proof(w, unmount_seen_first, false,
						       worker_exit_status) ||
	    s->entry.state != NTFSMAC_JOURNAL_STATE_OPEN) {
		res->report = NTFSMAC_SREPORT_UNCLEAN;
		sess_close(s);
		return NTFSMAC_DMN_OK;
	}
	if (s->sys->sync_device(s->sys->ctx, s->fd, &flush_ok) != NTFSMAC_DMN_OK) {
		res->report = NTFSMAC_SREPORT_UNCLEAN; /* a failed fsync: not clean */
		sess_close(s);
		return NTFSMAC_DMN_OK;
	}
	if (s->entry.marker == NTFSMAC_JOURNAL_MARKER_SET) {
		unsigned t = s->cfg.volmark_timeout_s != 0 ? s->cfg.volmark_timeout_s : 20u;

		memset(&run, 0, sizeof run);
		run.device_fd = s->fd;
		run.serial = s->entry.serial;
		run.total_sectors = s->entry.total_sectors;
		run.timeout_s = t;
		st = s->sys->volmark(s->sys->ctx, &run, NTFSMAC_VOLMARK_CLEAR, &exit_code);
		if (st != NTFSMAC_DMN_OK || exit_code != 0) {
			/* The flag stays on the disk and the entry stays: next attach is ro. */
			res->report = NTFSMAC_SREPORT_CLEAR_FAILED;
			res->clear_interrupted = st == NTFSMAC_DMN_E_TIMEOUT && exit_code == -2;
			sess_close(s);
			return NTFSMAC_DMN_OK;
		}
	}
	if (ntfsmac_journal_remove_if(s->j, &s->entry, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION,
				      s->sys->now(s->sys->ctx)) != NTFSMAC_JOURNAL_OK) {
		/* flag clear, entry present: next attach is ro until journal-clear (section 3.6 step 4) */
		res->report = NTFSMAC_SREPORT_UNCLEAN;
		sess_close(s);
		return NTFSMAC_DMN_OK;
	}
	s->has_entry = false;
	res->entry_removed = true;
	res->report = (!flush_ok || (w->flags & NTFSMAC_EW_SAW_W) != 0) ? NTFSMAC_SREPORT_CLEAN_NO_FLUSH
									 : NTFSMAC_SREPORT_CLEAN;
	sess_close(s);
	return NTFSMAC_DMN_OK;
}

void ntfsmac_session_free(ntfsmac_session *s)
{
	if (s == NULL)
		return;
	sess_close(s);
	free(s);
}

/* ------------------------------------------------------------------ */
/* A bound marker write outside a session (ADR-017 clear-repair)       */
/* ------------------------------------------------------------------ */

ntfsmac_dmn_status ntfsmac_session_marker_write(const ntfsmac_session_sys *sys,
						const ntfsmac_session_begin_in *in,
						ntfsmac_volmark_op op, unsigned timeout_s,
						int *exit_code)
{
	ntfsmac_disk_info di;
	ntfsmac_dev_id id_a, id_b;
	uint8_t blk_b[NTFSMAC_BLOCK0_BYTES], again[NTFSMAC_BLOCK0_BYTES];
	ntfsmac_volmark_run run;
	ntfsmac_dmn_status st, ds;
	int b = -1;

	if (exit_code != NULL)
		*exit_code = -3;
	if (!config_ok_sys(sys) || in == NULL || in->bsd == NULL || !ntfsmac_ctl_bsd_valid(in->bsd) ||
	    in->verdict == NULL || exit_code == NULL || timeout_s == 0) {
		if (in != NULL && in->probe_fd >= 0)
			(void)close(in->probe_fd);
		return NTFSMAC_DMN_E_INVAL;
	}

	/*
	 * The binding of section 4.1 step 6, against what was captured before the gate probe ran on
	 * this same descriptor A: the disk is the same object (BSD name, DAMediaPath), nobody has it
	 * mounted (Apple's driver included: the gate needs it unmounted), B is the same device, and
	 * the first 4096 bytes through B are the bytes read before the probe.
	 */
	memset(&di, 0, sizeof di);
	ds = sys->describe(sys->ctx, in->bsd, &di);
	if (ntfsmac_bind_check_disk(in->bsd, in->media_path, &di, ds) != NTFSMAC_BIND_OK ||
	    sys->dev_identity(sys->ctx, in->probe_fd, &id_a) != 0) {
		sys->close_fd(sys->ctx, in->probe_fd);
		return NTFSMAC_DMN_E_DENIED;
	}
	b = sys->open_device(sys->ctx, in->bsd, true);
	if (b < 0 || sys->dev_identity(sys->ctx, b, &id_b) != 0 ||
	    !ntfsmac_bind_same_device(&id_a, &id_b) || sys->read_block0(sys->ctx, b, blk_b) != 0 ||
	    memcmp(in->block0, blk_b, sizeof blk_b) != 0) {
		sys->close_fd(sys->ctx, in->probe_fd);
		if (b >= 0)
			sys->close_fd(sys->ctx, b);
		return NTFSMAC_DMN_E_DENIED;
	}
	sys->close_fd(sys->ctx, in->probe_fd);

	memset(&run, 0, sizeof run);
	run.device_fd = b;
	run.serial = in->verdict->serial;
	run.total_sectors = in->verdict->total_sectors;
	run.timeout_s = timeout_s;
	st = sys->volmark(sys->ctx, &run, op, exit_code);
	if (st == NTFSMAC_DMN_OK &&
	    (sys->read_block0(sys->ctx, b, again) != 0 || memcmp(again, in->block0, sizeof again) != 0))
		st = NTFSMAC_DMN_E_IO; /* sector 0 changed while volmark ran */
	sys->close_fd(sys->ctx, b);
	return st;
}
