/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-PRV-01..08: the privileged operations behind the five checks (src/daemon/ctl_journal.c;
 * ARCHITECTURE.md sections 3.2, 3.4, 3.5, ADR-017). Host test: the fresh probe, the volume
 * registry and the marker write are fakes; the journal is the real library on a temporary
 * directory. Checks 1 to 4 are tested with the dispatcher (tests/unit/ctl).
 *
 *   01 journal_clear refusals, each leaves the entry     05 label and serial confirmation
 *   02 journal_clear accepted, warnings, corrupt entry   06 gate names and reply shape
 *   03 live session on another disk (a clone)            07 repair sync_mirror (T-M4-16)
 *   04 repair --clear-dirty                              08 arguments
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "internal.h"
#include "ntfsmac/ctl_proto.h"

#define SERIAL UINT64_C(0x1122334455667788)
#define NOW0 INT64_C(1700000000)

typedef struct fake {
	bool known;
	ntfsmac_priv_volume vol;
	bool probe_fails;
	ntfsmac_probe_verdict verdict;
	uint64_t live_id;
	ntfsmac_dmn_status clear_status;
	int clear_exit;
	int probes, clears;
	bool swap_on_live, swap_in_clear, swapped;
	ntfsmac_journal *swap_j;
	char clear_bsd[32];
	uint64_t clear_serial;
	/* sync_mirror (T-M4-16) */
	ntfsmac_dmn_status sync_status;
	int sync_exit;
	int syncs;
	bool after_set;               /* the probe after the write returns after_verdict */
	bool after_probe_fails;
	ntfsmac_probe_verdict after_verdict;
	char sync_bsd[32];
	uint64_t sync_serial;
	int64_t later; /* seconds added to the clock */
} fake;

static bool f_volume(void *ctx, const char *bsd, ntfsmac_priv_volume *out)
{
	fake *f = ctx;

	(void)bsd;
	*out = f->vol;
	return f->known;
}

static ntfsmac_dmn_status f_probe(void *ctx, const char *bsd, ntfsmac_probe_verdict *out,
				  ntfsmac_priv_bind *bind)
{
	fake *f = ctx;

	(void)bsd;
	bind->fd = -1;
	f->probes++;
	if (f->syncs > 0 && f->after_probe_fails)
		return NTFSMAC_DMN_E_IO;
	if (f->probe_fails)
		return NTFSMAC_DMN_E_IO;
	*out = f->syncs > 0 && f->after_set ? f->after_verdict : f->verdict;
	return NTFSMAC_DMN_OK;
}

static void swap_entry(fake *f);

static bool f_live(void *ctx, uint64_t id)
{
	fake *f = ctx;

	if (f->swap_on_live)
		swap_entry(f);
	return f->live_id != 0 && f->live_id == id;
}

static void swap_entry(fake *f);

static ntfsmac_dmn_status f_clear(void *ctx, const char *bsd, const ntfsmac_probe_verdict *v,
				  ntfsmac_priv_bind *bind, int *exit_code)
{
	fake *f = ctx;

	(void)bind;
	if (f->swap_in_clear)
		swap_entry(f);
	f->clears++;
	snprintf(f->clear_bsd, sizeof f->clear_bsd, "%s", bsd);
	f->clear_serial = v->serial;
	*exit_code = f->clear_exit;
	return f->clear_status;
}

static ntfsmac_dmn_status f_sync(void *ctx, const char *bsd, const ntfsmac_probe_verdict *v,
				 ntfsmac_priv_bind *bind, int *exit_code)
{
	fake *f = ctx;

	(void)bind;
	f->syncs++;
	snprintf(f->sync_bsd, sizeof f->sync_bsd, "%s", bsd);
	f->sync_serial = v->serial;
	*exit_code = f->sync_exit;
	return f->sync_status;
}

/* A new session of the same serial starts: the entry is replaced by one of session 99. */
static void swap_entry(fake *f)
{
	ntfsmac_journal_entry e;

	if (f->swapped)
		return;
	f->swapped = true;
	CHECK_EQ_INT(ntfsmac_journal_remove(f->swap_j, SERIAL, 131072, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, NOW0 - 500),
		     NTFSMAC_JOURNAL_OK);
	memset(&e, 0, sizeof e);
	e.serial = SERIAL;
	snprintf(e.label, sizeof e.label, "My Disk");
	e.total_sectors = 131072;
	e.session_id = 99;
	e.created = e.updated = NOW0 - 1;
	e.logfile_lsn_at_start = 0x1234;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_EQ_INT(ntfsmac_journal_create(f->swap_j, &e), NTFSMAC_JOURNAL_OK);
}

static int64_t f_now(void *ctx)
{
	return NOW0 + ((fake *)ctx)->later;
}

static char g_dir[160];
static unsigned g_case;

static void rm_tree(const char *dir)
{
	char cmd[400];

	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); /* our own temporary directory */
	if (system(cmd) != 0)
		fprintf(stderr, "cleanup of %s failed\n", dir);
}

typedef struct rig {
	fake f;
	ntfsmac_priv_sys sys;
	ntfsmac_priv priv;
	ntfsmac_journal *j;
	char jdir[220];
	char reply[NTFSMAC_CTL_REPLY_MIN * 2];
} rig;

static ntfsmac_probe_verdict clean_verdict(void)
{
	ntfsmac_probe_verdict v;

	memset(&v, 0, sizeof v);
	v.is_ntfs = true;
	v.has_ntfs_version = true;
	v.ntfs_version_major = 3;
	v.ntfs_version_minor = 1;
	v.serial = SERIAL;
	v.total_sectors = 131072;
	v.cluster_size = 4096;
	v.sector_size = 512;
	v.logfile = NTFSMAC_PROBE_LOGFILE_CLEAN;
	v.has_logfile_lsn = true;
	v.logfile_lsn = 0x1234;
	v.hiberfile = NTFSMAC_PROBE_HIBERFILE_ABSENT;
	v.state = NTFSMAC_PROBE_STATE_CLEAN;
	snprintf(v.label, sizeof v.label, "My Disk");
	v.label_len = 7;
	return v;
}

static ntfsmac_probe_verdict dirty_verdict(void)
{
	ntfsmac_probe_verdict v = clean_verdict();

	v.state = NTFSMAC_PROBE_STATE_DIRTY_FLAG;
	v.volume_flags = NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_DIRTY);
	v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY);
	return v;
}

static void rig_init(rig *r)
{
	memset(r, 0, sizeof *r);
	snprintf(r->jdir, sizeof r->jdir, "%s/j%u", g_dir, ++g_case);
	CHECK_EQ_INT(ntfsmac_journal_open(r->jdir, &r->j), NTFSMAC_JOURNAL_OK);
	r->f.known = true;
	r->f.swap_j = r->j;
	r->f.verdict = clean_verdict();
	r->sys.ctx = &r->f;
	r->sys.volume = f_volume;
	r->sys.probe_fresh = f_probe;
	r->sys.session_live = f_live;
	r->sys.clear_dirty = f_clear;
	r->sys.sync_mirror = f_sync;
	r->sys.now = f_now;
	r->priv.sys = &r->sys;
	r->priv.journal = r->j;
}

static void rig_done(rig *r)
{
	ntfsmac_journal_close(r->j);
}

static void make_entry(rig *r, ntfsmac_journal_marker marker, ntfsmac_journal_state state,
		       uint64_t lsn, uint64_t session_id)
{
	ntfsmac_journal_entry e;

	memset(&e, 0, sizeof e);
	e.serial = SERIAL;
	snprintf(e.label, sizeof e.label, "My Disk");
	e.total_sectors = 131072;
	e.session_id = session_id;
	e.created = e.updated = NOW0 - 1000;
	e.logfile_lsn_at_start = lsn;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_EQ_INT(ntfsmac_journal_create(r->j, &e), NTFSMAC_JOURNAL_OK);
	if (marker != NTFSMAC_JOURNAL_MARKER_PENDING || state != NTFSMAC_JOURNAL_STATE_OPEN) {
		e.marker = marker;
		e.state = state;
		CHECK_EQ_INT(ntfsmac_journal_update(r->j, &e), NTFSMAC_JOURNAL_OK);
	}
}

static bool entry_exists(rig *r)
{
	ntfsmac_journal_entry e;

	return ntfsmac_journal_get(r->j, SERIAL, 131072, &e) == NTFSMAC_JOURNAL_OK;
}

static bool has(const rig *r, const char *s)
{
	return strstr(r->reply, s) != NULL;
}

static ntfsmac_ctl_peer peer(void)
{
	ntfsmac_ctl_peer p;

	memset(&p, 0, sizeof p);
	p.uid = 501;
	p.pid = 1234;
	p.admin = true;
	p.console_user = true;
	return p;
}

static ntfsmac_dmn_status jc(rig *r, const char *label, const char *serial)
{
	ntfsmac_ctl_peer p = peer();

	memset(r->reply, 'Z', sizeof r->reply);
	return ntfsmac_priv_journal_clear(&r->priv, &p, "disk4s1", label, serial, r->reply,
					  sizeof r->reply);
}

static ntfsmac_dmn_status rp(rig *r, const char *action, const char *label, const char *serial)
{
	ntfsmac_ctl_peer p = peer();

	memset(r->reply, 'Z', sizeof r->reply);
	return ntfsmac_priv_repair(&r->priv, &p, "disk4s1", action, label, serial, r->reply,
				   sizeof r->reply);
}

#define GOOD_LABEL "My Disk"
#define GOOD_SERIAL "55667788"

static int count_tomb_cb(uint64_t s, int64_t t, void *ctx)
{
	(void)s;
	(void)t;
	(*(unsigned *)ctx)++;
	return 0;
}

static unsigned tombstones(rig *r)
{
	unsigned n = 0;

	CHECK_EQ_INT(ntfsmac_journal_list_tombstones(r->j, count_tomb_cb, &n), NTFSMAC_JOURNAL_OK);
	return n;
}

/* ---------------- 01 ---------------- */

static void refused(rig *r, ntfsmac_dmn_status st, const char *reason)
{
	char want[96];

	snprintf(want, sizeof want, "\"reason\":\"%s\"", reason);
	if (st != NTFSMAC_DMN_E_DENIED || !has(r, want))
		fprintf(stderr, "expected refusal %s, got status %d reply %s\n", reason, (int)st, r->reply);
	CHECK_EQ_INT(st, NTFSMAC_DMN_E_DENIED);
	CHECK(has(r, want));
	CHECK(has(r, "\"ok\":false"));
	CHECK(strchr(r->reply, '\n') == NULL);
}

static void test_clear_refusals(void)
{
	rig r;

	/* no such volume */
	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1234, 9);
	r.f.known = false;
	refused(&r, jc(&r, GOOD_LABEL, GOOD_SERIAL), "no_volume");
	CHECK_EQ_INT(r.f.probes, 0);
	CHECK(entry_exists(&r));
	r.f.known = true;

	/* the fresh probe failed */
	r.f.probe_fails = true;
	refused(&r, jc(&r, GOOD_LABEL, GOOD_SERIAL), "probe_failed");
	CHECK(entry_exists(&r));
	r.f.probe_fails = false;

	/* the probe says dirty_flag, hibernated, unclean_log, corrupt: not exactly clean */
	{
		static const ntfsmac_probe_state states[] = {
			NTFSMAC_PROBE_STATE_DIRTY_FLAG, NTFSMAC_PROBE_STATE_UNCLEAN_LOG,
			NTFSMAC_PROBE_STATE_HIBERNATED, NTFSMAC_PROBE_STATE_CORRUPT,
			NTFSMAC_PROBE_STATE_UNKNOWN,    NTFSMAC_PROBE_STATE_MIRROR_SPLIT};
		size_t i;

		for (i = 0; i < sizeof states / sizeof states[0]; i++) {
			r.f.verdict.state = states[i];
			refused(&r, jc(&r, GOOD_LABEL, GOOD_SERIAL), "probe_not_clean");
			CHECK(entry_exists(&r));
		}
		r.f.verdict = clean_verdict();
	}

	/* mounted read/write */
	r.f.vol.mounted = true;
	r.f.vol.mounted_rw = true;
	refused(&r, jc(&r, GOOD_LABEL, GOOD_SERIAL), "mounted_rw");
	CHECK(entry_exists(&r));
	r.f.vol.mounted = r.f.vol.mounted_rw = false;

	/* wrong label, wrong serial, label of another case, empty */
	refused(&r, jc(&r, "My disk", GOOD_SERIAL), "confirmation");
	refused(&r, jc(&r, GOOD_LABEL, "55667789"), "confirmation");
	refused(&r, jc(&r, GOOD_LABEL, "00000000"), "confirmation");
	refused(&r, jc(&r, GOOD_LABEL, "5566778"), "confirmation");
	refused(&r, jc(&r, GOOD_LABEL, "55667788 "), "confirmation");
	refused(&r, jc(&r, "", GOOD_SERIAL), "confirmation");
	refused(&r, jc(&r, GOOD_LABEL, "aabbccdd"), "confirmation");
	CHECK(entry_exists(&r));
	CHECK_EQ_INT(tombstones(&r), 0);
	/* the serial must be the upper-case digits that `list` shows */
	refused(&r, jc(&r, GOOD_LABEL, "55667788\n"), "confirmation");
	rig_done(&r);

	/* no entry for this volume */
	rig_init(&r);
	refused(&r, jc(&r, GOOD_LABEL, GOOD_SERIAL), "no_entry");
	rig_done(&r);
}

/* ---------------- 02 ---------------- */

static void test_clear_accept(void)
{
	rig r;

	/* clean, entry: cleared; the entry is in the tombstones; no warning */
	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 9);
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(has(&r, "\"ok\":true") && has(&r, "\"result\":\"cleared\"") && has(&r, "\"warning\":\"none\""));
	CHECK(!entry_exists(&r));
	CHECK_EQ_INT(tombstones(&r), 1);
	CHECK_EQ_INT(r.f.clears, 0); /* journal-clear never writes the volume */
	rig_done(&r);

	/* mounted read-only is fine (the mount stays as it is) */
	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 9);
	r.f.vol.mounted = true;
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	rig_done(&r);

	/* chkdsk-flag (state clean: informational flag only, N-04) with an entry: accepted */
	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 9);
	r.f.verdict.volume_flags = NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_MODIFIED_BY_CHKDSK);
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	rig_done(&r);

	/* the stronger words of section 3.5 path 3 */
	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_WRITE_ERROR, 0x1000, 9);
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(has(&r, "\"warning\":\"write_error\""));
	rig_done(&r);
	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1234, 9); /* LSN unchanged */
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(has(&r, "\"warning\":\"lost_flush\""));
	rig_done(&r);
	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_WRITE_ERROR, 0x1234, 9);
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(has(&r, "\"warning\":\"write_error\"")); /* write_error wins */
	rig_done(&r);

	/* a corrupt entry may still be moved away (section 3.5), with the strong warning */
	rig_init(&r);
	{
		char path[300];
		FILE *fp;

		snprintf(path, sizeof path, "%s/%016llX", r.jdir, (unsigned long long)SERIAL);
		fp = fopen(path, "w");
		CHECK(fp != NULL);
		if (fp != NULL) {
			fputs("not an entry", fp);
			fclose(fp);
			chmod(path, 0600);
		}
	}
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(has(&r, "\"warning\":\"write_error\""));
	CHECK(!entry_exists(&r));
	rig_done(&r);

	/* a tombstone with the same name (same second) makes the removal fail: said, entry kept */
	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 9);
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 10);
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_E_IO);
	CHECK(has(&r, "\"error\":\"failed\"") && has(&r, "\"reason\":\"journal\""));
	CHECK(entry_exists(&r));
	rig_done(&r);
}

/* ---------------- 03 ---------------- */

static void test_clone(void)
{
	rig r;

	/* a second disk with the same serial (a clone): the entry belongs to a live rw session */
	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 4242);
	r.f.live_id = 4242;
	refused(&r, jc(&r, GOOD_LABEL, GOOD_SERIAL), "live_session");
	CHECK(entry_exists(&r) && tombstones(&r) == 0);
	/* the session ends: the same request is accepted */
	r.f.live_id = 0;
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	rig_done(&r);
}

/* ---------------- 03b ---------------- */

/* The entry the gate checked is replaced by a new session's entry before the removal. */
static void test_entry_race(void)
{
	rig r;
	ntfsmac_journal_entry e;

	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 9);
	r.f.swap_on_live = true; /* the registry is asked between the gate and the removal */
	refused(&r, jc(&r, GOOD_LABEL, GOOD_SERIAL), "entry_changed");
	CHECK(ntfsmac_journal_get(r.j, SERIAL, 131072, &e) == NTFSMAC_JOURNAL_OK && e.session_id == 99);
	rig_done(&r);

	/* repair: the entry is swapped while volmark runs; the repair stands, the new entry stays */
	rig_init(&r);
	r.f.verdict = dirty_verdict();
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 9);
	r.f.swap_in_clear = true;
	CHECK_EQ_INT(rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(has(&r, "\"journal_entry\":\"kept\""));
	CHECK(ntfsmac_journal_get(r.j, SERIAL, 131072, &e) == NTFSMAC_JOURNAL_OK && e.session_id == 99);
	rig_done(&r);

	/* ntfsmac_journal_remove_if directly: same session and creation time only */
	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 9);
	CHECK(ntfsmac_journal_get(r.j, SERIAL, 131072, &e) == NTFSMAC_JOURNAL_OK);
	e.session_id = 10;
	CHECK_EQ_INT(ntfsmac_journal_remove_if(r.j, &e, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, NOW0),
		     NTFSMAC_JOURNAL_ERR_PATH_CONDITION);
	e.session_id = 9;
	e.created++;
	CHECK_EQ_INT(ntfsmac_journal_remove_if(r.j, &e, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, NOW0),
		     NTFSMAC_JOURNAL_ERR_PATH_CONDITION);
	e.created--;
	CHECK_EQ_INT(ntfsmac_journal_remove_if(r.j, &e, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, NOW0),
		     NTFSMAC_JOURNAL_OK);
	CHECK_EQ_INT(ntfsmac_journal_remove_if(r.j, &e, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, NOW0 + 1),
		     NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	rig_done(&r);
}

/* ---------------- 04 ---------------- */

static void test_repair(void)
{
	rig r;

	/* accepted: dirty_flag, nothing else, not mounted */
	rig_init(&r);
	r.f.verdict = dirty_verdict();
	CHECK_EQ_INT(rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(has(&r, "\"result\":\"repaired\"") && has(&r, "\"journal_entry\":\"none\"") &&
	      has(&r, "\"warning\":\"none\""));
	CHECK_EQ_INT(r.f.clears, 1);
	CHECK(strcmp(r.f.clear_bsd, "disk4s1") == 0 && r.f.clear_serial == SERIAL);
	rig_done(&r);

	/* dirty-chkdsk-flag keeps its informational flag: accepted */
	rig_init(&r);
	r.f.verdict = dirty_verdict();
	r.f.verdict.volume_flags |= NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_MODIFIED_BY_CHKDSK);
	CHECK_EQ_INT(rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	rig_done(&r);

	/* with a journal entry: removed (path 4), and the extra warning */
	rig_init(&r);
	r.f.verdict = dirty_verdict();
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 9);
	CHECK_EQ_INT(rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(has(&r, "\"journal_entry\":\"removed\"") && has(&r, "\"warning\":\"journal_entry\""));
	CHECK(!entry_exists(&r) && tombstones(&r) == 1);
	rig_done(&r);

	/* the entry belongs to a live session of another disk: refused before any write */
	rig_init(&r);
	r.f.verdict = dirty_verdict();
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 77);
	r.f.live_id = 77;
	refused(&r, rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), "live_session");
	CHECK_EQ_INT(r.f.clears, 0);
	CHECK(entry_exists(&r));
	rig_done(&r);

	/* refusals leave the image alone (clear_dirty is never called) */
	rig_init(&r);
	r.f.verdict = dirty_verdict();
	r.f.verdict.state = NTFSMAC_PROBE_STATE_CLEAN;
	refused(&r, rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), "probe_not_dirty_flag");
	r.f.verdict = dirty_verdict();
	r.f.verdict.state = NTFSMAC_PROBE_STATE_UNCLEAN_LOG;
	r.f.verdict.logfile = NTFSMAC_PROBE_LOGFILE_UNCLEAN;
	refused(&r, rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), "probe_not_dirty_flag");
	r.f.verdict = dirty_verdict();
	r.f.verdict.logfile = NTFSMAC_PROBE_LOGFILE_UNCLEAN;
	refused(&r, rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), "log_not_clean");
	r.f.verdict = dirty_verdict();
	r.f.verdict.hiberfile = NTFSMAC_PROBE_HIBERFILE_HIBERNATED;
	refused(&r, rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), "hiberfile");
	r.f.verdict = dirty_verdict();
	r.f.verdict.volume_flags |= NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_REPAIR_OBJECT_ID);
	refused(&r, rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), "flags");
	r.f.verdict = dirty_verdict();
	r.f.vol.mounted = true; /* mounted in any mode */
	refused(&r, rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), "mounted");
	r.f.vol.mounted = false;
	refused(&r, rp(&r, "clear_dirty", "wrong", GOOD_SERIAL), "confirmation");
	refused(&r, rp(&r, "clear_dirty", GOOD_LABEL, "00000000"), "confirmation");
	r.f.known = false;
	refused(&r, rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), "no_volume");
	r.f.known = true;
	r.f.probe_fails = true;
	refused(&r, rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), "probe_failed");
	CHECK_EQ_INT(r.f.clears, 0);
	rig_done(&r);

	/* the bound write refuses (another mount, a swapped device): reported, nothing removed */
	rig_init(&r);
	r.f.verdict = dirty_verdict();
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 9);
	r.f.clear_status = NTFSMAC_DMN_E_DENIED;
	refused(&r, rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), "binding");
	CHECK(entry_exists(&r));
	/* volmark failed (exit 13 or 15, timeout): failed, entry kept */
	r.f.clear_status = NTFSMAC_DMN_E_IO;
	r.f.clear_exit = 13;
	CHECK_EQ_INT(rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_E_IO);
	CHECK(has(&r, "\"error\":\"failed\"") && has(&r, "\"reason\":\"volmark_failed\""));
	CHECK(entry_exists(&r));
	r.f.clear_status = NTFSMAC_DMN_E_TIMEOUT;
	CHECK_EQ_INT(rp(&r, "clear_dirty", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_E_TIMEOUT);
	CHECK(entry_exists(&r));
	rig_done(&r);
}

/* ---------------- 05 ---------------- */

static void test_confirmation(void)
{
	rig r;

	/* the typed label is the sanitised one `list` shows: a hostile label is typed as shown */
	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 9);
	snprintf(r.f.verdict.label, sizeof r.f.verdict.label, "A\x1b[2JB");
	r.f.verdict.label_len = strlen(r.f.verdict.label);
	refused(&r, jc(&r, "A\x1b[2JB", GOOD_SERIAL), "confirmation"); /* the raw bytes do not match */
	CHECK(strchr(r.reply, '\x1b') == NULL);                       /* and are never echoed */
	CHECK_EQ_INT(jc(&r, "A__2JB", GOOD_SERIAL), NTFSMAC_DMN_OK);
	rig_done(&r);

	/* an empty label becomes the fallback NTFS-<last 8 hex digits> */
	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 9);
	r.f.verdict.label[0] = '\0';
	r.f.verdict.label_len = 0;
	CHECK_EQ_INT(jc(&r, "NTFS-55667788", GOOD_SERIAL), NTFSMAC_DMN_OK);
	rig_done(&r);
}

/* The `journal` member of a status reply: what the CLI shows before the confirmation. */
static void test_journal_member(void)
{
	ntfsmac_journal_entry e;
	ntfsmac_probe_verdict v = clean_verdict();
	char buf[256];

	memset(&e, 0, sizeof e);
	e.serial = SERIAL;
	e.created = 1700000000;
	e.logfile_lsn_at_start = 0x1234;
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK(ntfsmac_priv_journal_member(&e, &v, buf, sizeof buf) > 0);
	CHECK(strstr(buf, "\"marker\":\"set\"") && strstr(buf, "\"warning\":\"lost_flush\"") &&
	      strstr(buf, "\"left_open\":\"2023-11-14T22:13:20Z\""));
	e.state = NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
	CHECK(ntfsmac_priv_journal_member(&e, &v, buf, sizeof buf) > 0 && strstr(buf, "\"warning\":\"write_error\""));
	e.state = NTFSMAC_JOURNAL_STATE_OPEN;
	v.logfile_lsn = 0x9999;
	CHECK(ntfsmac_priv_journal_member(&e, &v, buf, sizeof buf) > 0 && strstr(buf, "\"warning\":\"none\""));
	CHECK(ntfsmac_priv_journal_member(&e, NULL, buf, sizeof buf) > 0);
	CHECK_EQ_INT(ntfsmac_priv_journal_member(NULL, &v, buf, sizeof buf), 0);
	CHECK_EQ_INT(ntfsmac_priv_journal_member(&e, &v, buf, 10), 0);
}

/* ---------------- 06, 07, 08 ---------------- */

/* ---------------- 07 ---------------- */

static ntfsmac_probe_verdict split_verdict(void)
{
	ntfsmac_probe_verdict v = clean_verdict();

	v.state = NTFSMAC_PROBE_STATE_MIRROR_SPLIT;
	v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_MFTMIRR_STALE);
	return v;
}

static void test_sync_mirror(void)
{
	rig r;
	size_t i;

	/* accepted: the write runs once, then a second probe whose state is in the reply */
	rig_init(&r);
	r.f.verdict = split_verdict();
	r.f.after_set = true;
	r.f.after_verdict = clean_verdict();
	CHECK_EQ_INT(rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(has(&r, "\"ok\":true") && has(&r, "\"result\":\"repaired\"") && has(&r, "\"state\":\"clean\"") &&
	      has(&r, "\"journal_entry\":\"untouched\""));
	CHECK_EQ_INT(r.f.syncs, 1);
	CHECK_EQ_INT(r.f.clears, 0);
	CHECK_EQ_INT(r.f.probes, 2);
	CHECK(strcmp(r.f.sync_bsd, "disk4s1") == 0 && r.f.sync_serial == SERIAL);
	rig_done(&r);

	/* a split with the dirty flag set (record 3): the volume comes back dirty_flag, as reported */
	rig_init(&r);
	r.f.verdict = split_verdict();
	r.f.verdict.reason_codes |= NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY) |
				    NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC);
	r.f.verdict.volume_flags = NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_DIRTY);
	r.f.after_set = true;
	r.f.after_verdict = dirty_verdict();
	CHECK_EQ_INT(rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(has(&r, "\"state\":\"dirty_flag\""));
	rig_done(&r);

	/* the journal is untouched: an entry (also of a live session) stays, no tombstone */
	rig_init(&r);
	r.f.verdict = split_verdict();
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_WRITE_ERROR, 0x1000, 77);
	r.f.live_id = 77;
	CHECK_EQ_INT(rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(entry_exists(&r) && tombstones(&r) == 0);
	CHECK(has(&r, "\"journal_entry\":\"untouched\""));
	rig_done(&r);

	/* the probe after the write fails: the repair happened, the state is unknown */
	rig_init(&r);
	r.f.verdict = split_verdict();
	r.f.after_probe_fails = true;
	CHECK_EQ_INT(rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(has(&r, "\"state\":\"unknown\""));
	rig_done(&r);

	/* refusals: the write is never called, and only one probe ran */
	rig_init(&r);
	r.f.known = false;
	refused(&r, rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), "no_volume");
	CHECK_EQ_INT(r.f.probes, 0);
	r.f.known = true;
	r.f.probe_fails = true;
	refused(&r, rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), "probe_failed");
	r.f.probe_fails = false;
	r.f.verdict = split_verdict();
	r.f.vol.mounted = true;
	refused(&r, rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), "mounted");
	r.f.vol.mounted = false;
	refused(&r, rp(&r, "sync_mirror", "Other", GOOD_SERIAL), "confirmation");
	refused(&r, rp(&r, "sync_mirror", GOOD_LABEL, "00000000"), "confirmation");
	r.f.verdict.logfile = NTFSMAC_PROBE_LOGFILE_UNCLEAN;
	refused(&r, rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), "log_not_clean");
	r.f.verdict = split_verdict();
	r.f.verdict.hiberfile = NTFSMAC_PROBE_HIBERFILE_HIBERNATED;
	refused(&r, rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), "hiberfile");
	/* other states and other codes beside MFTMIRR_STALE */
	{
		static const ntfsmac_probe_state states[] = {
			NTFSMAC_PROBE_STATE_CLEAN,      NTFSMAC_PROBE_STATE_DIRTY_FLAG,
			NTFSMAC_PROBE_STATE_CORRUPT,    NTFSMAC_PROBE_STATE_UNCLEAN_LOG,
			NTFSMAC_PROBE_STATE_HIBERNATED, NTFSMAC_PROBE_STATE_UNKNOWN};

		for (i = 0; i < sizeof states / sizeof states[0]; i++) {
			r.f.verdict = split_verdict();
			r.f.verdict.state = states[i];
			refused(&r, rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), "probe_not_mirror_split");
		}
	}
	r.f.verdict = split_verdict();
	r.f.verdict.reason_codes |= NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_LOG_UNCLEAN);
	refused(&r, rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), "probe_not_mirror_split");
	CHECK_EQ_INT(r.f.syncs, 0);
	rig_done(&r);

	/* volmark refused before `w` (exits 10, 13, pre-write timeout): denied, nothing written, state shown */
	{
		static const int refused_exit[] = { 10, 13, -1 };

		for (i = 0; i < sizeof refused_exit / sizeof refused_exit[0]; i++) {
			rig_init(&r);
			r.f.verdict = split_verdict();
			r.f.sync_status = refused_exit[i] == -1 ? NTFSMAC_DMN_E_TIMEOUT : NTFSMAC_DMN_E_IO;
			r.f.sync_exit = refused_exit[i];
			CHECK_EQ_INT(rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_E_DENIED);
			CHECK(has(&r, "\"reason\":\"volmark_refused\"") && has(&r, "\"state\":\"mirror_split\""));
			CHECK_EQ_INT(r.f.syncs, 1);
			CHECK_EQ_INT(r.f.probes, 2); /* the gate probe and the fresh one */
			rig_done(&r);
		}
	}
	/* the binding failed in volmark (15) or in the daemon: reason binding */
	rig_init(&r);
	r.f.verdict = split_verdict();
	r.f.sync_status = NTFSMAC_DMN_E_IO;
	r.f.sync_exit = 15;
	refused(&r, rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), "binding");
	CHECK(has(&r, "\"state\":"));
	r.f.syncs = 0;
	r.f.sync_status = NTFSMAC_DMN_E_DENIED;
	r.f.sync_exit = -3;
	refused(&r, rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), "binding");
	rig_done(&r);

	/* interrupted or failed after `w` (timeout -2, exits 12 and 14, sector 0 changed, busy): failed, may be torn */
	{
		static const struct {
			ntfsmac_dmn_status st;
			int exit;
		} after_w[] = { { NTFSMAC_DMN_E_TIMEOUT, -2 }, { NTFSMAC_DMN_E_IO, 12 }, { NTFSMAC_DMN_E_IO, 14 },
				{ NTFSMAC_DMN_E_IO, 0 }, { NTFSMAC_DMN_E_BUSY, -4 } };

		for (i = 0; i < sizeof after_w / sizeof after_w[0]; i++) {
			rig_init(&r);
			r.f.verdict = split_verdict();
			r.f.sync_status = after_w[i].st;
			r.f.sync_exit = after_w[i].exit;
			r.f.after_set = true;
			r.f.after_verdict = split_verdict();
			r.f.after_verdict.state = NTFSMAC_PROBE_STATE_CORRUPT;
			CHECK_EQ_INT(rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), after_w[i].st);
			CHECK(has(&r, "\"error\":\"failed\"") && has(&r, "\"reason\":\"volmark_failed\"") &&
			      has(&r, "\"state\":\"corrupt\""));
			CHECK(!has(&r, "repaired"));
			CHECK_EQ_INT(r.f.probes, 2);
			rig_done(&r);
		}
	}

	/* a daemon without the write seam answers "not implemented" and probes nothing */
	rig_init(&r);
	r.f.verdict = split_verdict();
	r.sys.sync_mirror = NULL;
	CHECK_EQ_INT(rp(&r, "sync_mirror", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_E_NOTIMPL);
	CHECK_EQ_INT(r.f.probes, 0);
	rig_done(&r);
}

static void test_names_and_args(void)
{
	rig r;
	int g;
	ntfsmac_ctl_peer p = peer();

	for (g = 0; g <= (int)NTFSMAC_GATE_PROBE_NOT_MIRROR_SPLIT; g++) {
		const char *n = ntfsmac_priv_gate_name((ntfsmac_policy_gate)g);
		int k;

		CHECK(n != NULL && n[0] != '\0');
		for (k = 0; k < g; k++)
			CHECK(strcmp(n, ntfsmac_priv_gate_name((ntfsmac_policy_gate)k)) != 0);
	}
	CHECK(ntfsmac_priv_gate_name((ntfsmac_policy_gate)99) != NULL);

	/* an action this daemon does not know: nothing is probed, nothing is written */
	rig_init(&r);
	r.f.verdict = dirty_verdict();
	CHECK_EQ_INT(rp(&r, "weird", GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_E_NOTIMPL);
	CHECK_EQ_INT(r.f.probes + r.f.clears + r.f.syncs, 0);
	rig_done(&r);

	/* NULL and empty arguments fail closed */
	rig_init(&r);
	CHECK_EQ_INT(ntfsmac_priv_journal_clear(NULL, &p, "disk4s1", "L", "00000001", r.reply, sizeof r.reply),
		     NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_priv_journal_clear(&r.priv, NULL, "disk4s1", "L", "00000001", r.reply, sizeof r.reply),
		     NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_priv_journal_clear(&r.priv, &p, NULL, "L", "00000001", r.reply, sizeof r.reply),
		     NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_priv_journal_clear(&r.priv, &p, "disk4s1", NULL, "00000001", r.reply, sizeof r.reply),
		     NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_priv_journal_clear(&r.priv, &p, "disk4s1", "L", "00000001", NULL, 0),
		     NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_priv_repair(&r.priv, &p, "disk4s1", NULL, "L", "00000001", r.reply, sizeof r.reply),
		     NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_priv_repair(NULL, &p, "disk4s1", "clear_dirty", "L", "00000001", r.reply, sizeof r.reply),
		     NTFSMAC_DMN_E_INVAL);
	r.priv.journal = NULL;
	CHECK_EQ_INT(ntfsmac_priv_journal_clear(&r.priv, &p, "disk4s1", "L", "00000001", r.reply, sizeof r.reply),
		     NTFSMAC_DMN_E_INVAL);
	r.priv.journal = r.j;
	CHECK_EQ_INT(r.f.probes, 0);
	rig_done(&r);
}

/*
 * U-DMN-PRV-ID (T-M7-13): two volumes with one serial and different total_sectors each have their
 * entry; journal-clear of one clears that one and leaves the other; a volume whose only entry with
 * its serial is another volume's has nothing to clear.
 */
static void test_identity(void)
{
	rig r;
	ntfsmac_journal_entry e;
	const uint64_t other = 262144;

	rig_init(&r);
	make_entry(&r, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN, 0x1000, 9);
	memset(&e, 0, sizeof e);
	e.serial = SERIAL;
	snprintf(e.label, sizeof e.label, "My Disk");
	e.total_sectors = other;
	e.session_id = 10;
	e.created = e.updated = NOW0 - 900;
	e.logfile_lsn_at_start = 0x1000;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_EQ_INT(ntfsmac_journal_create(r.j, &e), NTFSMAC_JOURNAL_OK);
	/* the probed volume is (SERIAL, 131072): its entry goes, the other one stays */
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(has(&r, "\"result\":\"cleared\""));
	CHECK(!entry_exists(&r));
	CHECK(ntfsmac_journal_get(r.j, SERIAL, other, &e) == NTFSMAC_JOURNAL_OK && e.session_id == 10);
	/* again for the same volume: no entry of its own, the other volume's is not touched */
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_E_DENIED);
	CHECK(has(&r, "no_entry"));
	CHECK(ntfsmac_journal_get(r.j, SERIAL, other, &e) == NTFSMAC_JOURNAL_OK && e.session_id == 10);
	/* the other volume clears its own (a second later: tombstones are named by the second) */
	r.f.verdict.total_sectors = other;
	r.f.later = 1;
	CHECK_EQ_INT(jc(&r, GOOD_LABEL, GOOD_SERIAL), NTFSMAC_DMN_OK);
	CHECK(ntfsmac_journal_get(r.j, SERIAL, other, &e) == NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	CHECK_EQ_INT(tombstones(&r), 2);
	rig_done(&r);
}

int main(void)
{
	const char *t = getenv("TMPDIR");

	snprintf(g_dir, sizeof g_dir, "%s%sntfsmac-prv-XXXXXX", t != NULL ? t : "/tmp",
		 (t != NULL && t[strlen(t) - 1] == '/') ? "" : "/");
	CHECK(mkdtemp(g_dir) != NULL);
	test_clear_refusals();
	test_clear_accept();
	test_identity();
	test_clone();
	test_entry_race();
	test_repair();
	test_sync_mirror();
	test_confirmation();
	test_names_and_args();
	test_journal_member();
	rm_tree(g_dir);
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
