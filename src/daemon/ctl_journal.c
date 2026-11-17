/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The privileged requests `journal_clear` and `repair --clear-dirty` (T-M4-14,
 * docs/ARCHITECTURE.md sections 3.2, 3.4, 3.5, ADR-017, ADR-019).
 *
 * By the time these functions run, checks 1 to 4 of ADR-019 have passed on the XPC door. What is
 * left is check 5: the volume exists, a probe run for this request shows what the gate in
 * section 3.2 needs, and the typed label and serial match what a fresh probe says (a guard
 * against mistakes: `list` publishes both). Then the one write: the journal entry moves to the
 * tombstones, or volmark clears the dirty flag.
 *
 * Every refusal is `{"ok":false,"error":"denied","reason":"<gate>"}` with a fixed word; no text
 * taken from a disk or from the request is echoed. The warnings of section 3.5 go back as fixed
 * words in the success reply, for the CLI to turn into text.
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "internal.h"
#include "sanitize.h"

/* One privileged request at a time: probe, gate and write form one step. */
static pthread_mutex_t g_priv_mu = PTHREAD_MUTEX_INITIALIZER;

const char *ntfsmac_priv_gate_name(ntfsmac_policy_gate g)
{
	switch (g) {
	case NTFSMAC_GATE_ALLOWED:
		return "allowed";
	case NTFSMAC_GATE_INVALID_INPUT:
		return "invalid_input";
	case NTFSMAC_GATE_NO_ENTRY:
		return "no_entry";
	case NTFSMAC_GATE_PROBE_NOT_CLEAN:
		return "probe_not_clean";
	case NTFSMAC_GATE_MOUNTED_RW:
		return "mounted_rw";
	case NTFSMAC_GATE_LIVE_SESSION:
		return "live_session";
	case NTFSMAC_GATE_PROBE_NOT_DIRTY_FLAG:
		return "probe_not_dirty_flag";
	case NTFSMAC_GATE_LOG_NOT_CLEAN:
		return "log_not_clean";
	case NTFSMAC_GATE_HIBERFILE:
		return "hiberfile";
	case NTFSMAC_GATE_FLAGS:
		return "flags";
	case NTFSMAC_GATE_MOUNTED:
		return "mounted";
	case NTFSMAC_GATE_PEER:
		return "peer";
	case NTFSMAC_GATE_CONFIRMATION:
		return "confirmation";
	case NTFSMAC_GATE_PROBE_NOT_MIRROR_SPLIT:
		return "probe_not_mirror_split";
	}
	return "invalid_input";
}

void ntfsmac_priv_release_bind(const ntfsmac_priv *p, ntfsmac_priv_bind *bind)
{
	if (p->sys->bind_release != NULL)
		p->sys->bind_release(p->sys->ctx, bind);
	bind->fd = -1;
}

ntfsmac_dmn_status ntfsmac_priv_refuse(char *reply, size_t cap, const char *reason)
{
	snprintf(reply, cap, "{\"ok\":false,\"error\":\"denied\",\"reason\":\"%s\"}", reason);
	return NTFSMAC_DMN_E_DENIED;
}

ntfsmac_dmn_status ntfsmac_priv_failed(char *reply, size_t cap, ntfsmac_dmn_status st,
				       const char *reason)
{
	snprintf(reply, cap, "{\"ok\":false,\"error\":\"failed\",\"status\":\"%s\",\"reason\":\"%s\"}",
		 ntfsmac_dmn_status_name(st), reason);
	return st;
}

/* The typed label is the sanitised label `list` shows; the serial is its last 8 hex digits. */
bool ntfsmac_priv_confirmation_ok(const ntfsmac_probe_verdict *v, const char *label,
				  const char *serial)
{
	char want_label[NTFSMAC_LABEL_PART_SIZE];
	char want_serial[16];

	if (ntfsmac_label_sanitize(v->label, v->label_len, v->serial, want_label,
				   sizeof want_label) == 0)
		return false;
	snprintf(want_serial, sizeof want_serial, "%08" PRIX32, (uint32_t)(v->serial & 0xFFFFFFFFu));
	return strcmp(label, want_label) == 0 && strcmp(serial, want_serial) == 0;
}

/* The entry for the verdict's serial; a corrupt or unreadable one stands in (section 3.5). */
static const ntfsmac_journal_entry *load_entry(ntfsmac_journal *j, const ntfsmac_probe_verdict *v,
					       ntfsmac_journal_entry *buf, bool *present)
{
	ntfsmac_journal_status js = ntfsmac_journal_get(j, v->serial, buf);

	*present = true;
	if (js == NTFSMAC_JOURNAL_OK)
		return buf;
	if (js == NTFSMAC_JOURNAL_ERR_NOT_FOUND) {
		*present = false;
		return NULL;
	}
	memset(buf, 0, sizeof *buf);
	buf->serial = v->serial;
	buf->marker = NTFSMAC_JOURNAL_MARKER_FAILED;
	buf->state = NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
	return buf;
}

static const char *warning_name(ntfsmac_policy_clear_warning w);

size_t ntfsmac_priv_journal_member(const ntfsmac_journal_entry *e, const ntfsmac_probe_verdict *last,
				   char *buf, size_t cap)
{
	static const char *const marker[] = {"pending", "set", "failed", "none"};
	struct tm tm;
	time_t t;
	char when[32];
	int n;

	if (e == NULL || buf == NULL || (unsigned)e->marker > NTFSMAC_JOURNAL_MARKER_NONE)
		return 0;
	t = (time_t)e->created;
	if (gmtime_r(&t, &tm) == NULL || strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
		snprintf(when, sizeof when, "unknown");
	n = snprintf(buf, cap,
		     "\"journal\":{\"marker\":\"%s\",\"state\":\"%s\",\"warning\":\"%s\",\"left_open\":\"%s\"}",
		     marker[e->marker], e->state == NTFSMAC_JOURNAL_STATE_WRITE_ERROR ? "write_error" : "open",
		     warning_name(ntfsmac_policy_journal_clear_warning(e, last)), when);
	return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static const char *warning_name(ntfsmac_policy_clear_warning w)
{
	return w == NTFSMAC_CLEAR_WARNING_WRITE_ERROR  ? "write_error"
	       : w == NTFSMAC_CLEAR_WARNING_LOST_FLUSH ? "lost_flush"
							: "none";
}

ntfsmac_dmn_status ntfsmac_priv_journal_clear(void *ctx, const ntfsmac_ctl_peer *peer,
					      const char *bsd, const char *confirm_label,
					      const char *confirm_serial, char *reply, size_t cap)
{
	ntfsmac_priv *p = ctx;
	ntfsmac_priv_volume vol;
	ntfsmac_probe_verdict *v;
	ntfsmac_priv_bind bind;
	ntfsmac_journal_entry e;
	const ntfsmac_journal_entry *ep;
	ntfsmac_policy_journal_clear_input in;
	ntfsmac_policy_gate gate;
	ntfsmac_dmn_status st;
	ntfsmac_journal_status js;
	bool present;
	static ntfsmac_probe_verdict verdict; /* big (about 10 KiB); serialised by g_priv_mu */

	if (p == NULL || p->sys == NULL || p->journal == NULL || peer == NULL || bsd == NULL ||
	    confirm_label == NULL || confirm_serial == NULL || reply == NULL || cap == 0)
		return NTFSMAC_DMN_E_INVAL;
	pthread_mutex_lock(&g_priv_mu);
	v = &verdict;
	memset(&vol, 0, sizeof vol);
	if (!p->sys->volume(p->sys->ctx, bsd, &vol)) {
		st = ntfsmac_priv_refuse(reply, cap, "no_volume");
		goto out;
	}
	memset(v, 0, sizeof *v);
	bind.fd = -1;
	if (p->sys->probe_fresh(p->sys->ctx, bsd, v, &bind) != NTFSMAC_DMN_OK) {
		ntfsmac_priv_release_bind(p, &bind);
		st = ntfsmac_priv_refuse(reply, cap, "probe_failed");
		goto out;
	}
	ntfsmac_priv_release_bind(p, &bind); /* journal-clear writes nothing to the volume */
	ep = load_entry(p->journal, v, &e, &present);

	memset(&in, 0, sizeof in);
	in.entry = ep;
	in.probe = v;
	in.mounted_rw = vol.mounted_rw;
	in.live_session_owns_entry = present && e.session_id != 0 &&
				     p->sys->session_live(p->sys->ctx, e.session_id);
	in.peer_authorised = true; /* checks 1 to 4 ran before this function was called */
	in.confirmation_ok = ntfsmac_priv_confirmation_ok(v, confirm_label, confirm_serial);
	gate = ntfsmac_policy_gate_journal_clear(&in);
	if (gate != NTFSMAC_GATE_ALLOWED) {
		st = ntfsmac_priv_refuse(reply, cap, ntfsmac_priv_gate_name(gate));
		goto out;
	}

	/* Only the entry the gate looked at: a new session of this serial may have started since. */
	js = ntfsmac_journal_remove_if(p->journal, ep, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR,
				       p->sys->now(p->sys->ctx));
	if (js == NTFSMAC_JOURNAL_ERR_PATH_CONDITION || js == NTFSMAC_JOURNAL_ERR_NOT_FOUND) {
		st = ntfsmac_priv_refuse(reply, cap, "entry_changed");
		goto out;
	}
	if (js != NTFSMAC_JOURNAL_OK) {
		st = ntfsmac_priv_failed(reply, cap, NTFSMAC_DMN_E_IO, "journal");
		goto out;
	}
	snprintf(reply, cap, "{\"ok\":true,\"result\":\"cleared\",\"warning\":\"%s\"}",
		 warning_name(ntfsmac_policy_journal_clear_warning(ep, v)));
	st = NTFSMAC_DMN_OK;
out:
	pthread_mutex_unlock(&g_priv_mu);
	return st;
}

ntfsmac_dmn_status ntfsmac_priv_repair(void *ctx, const ntfsmac_ctl_peer *peer, const char *bsd,
				       const char *action, const char *confirm_label,
				       const char *confirm_serial, char *reply, size_t cap)
{
	ntfsmac_priv *p = ctx;
	ntfsmac_priv_volume vol;
	ntfsmac_probe_verdict *v;
	ntfsmac_priv_bind bind;
	ntfsmac_journal_entry e;
	ntfsmac_policy_repair_input in;
	ntfsmac_policy_gate gate;
	ntfsmac_dmn_status st, ws;
	bool present, removed = false;
	int vexit = -3;
	static ntfsmac_probe_verdict verdict;

	if (p == NULL || p->sys == NULL || p->journal == NULL || peer == NULL || bsd == NULL ||
	    action == NULL || confirm_label == NULL || confirm_serial == NULL || reply == NULL ||
	    cap == 0)
		return NTFSMAC_DMN_E_INVAL;
	if (strcmp(action, "sync_mirror") == 0) {
		ntfsmac_dmn_status ms;

		pthread_mutex_lock(&g_priv_mu);
		ms = ntfsmac_priv_repair_mirror(p, bsd, confirm_label, confirm_serial, reply, cap);
		pthread_mutex_unlock(&g_priv_mu);
		return ms;
	}
	if (strcmp(action, "clear_dirty") != 0) {
		reply[0] = '\0'; /* not an action of this daemon: the dispatcher answers "not implemented" */
		return NTFSMAC_DMN_E_NOTIMPL;
	}
	pthread_mutex_lock(&g_priv_mu);
	v = &verdict;
	memset(&vol, 0, sizeof vol);
	if (!p->sys->volume(p->sys->ctx, bsd, &vol)) {
		st = ntfsmac_priv_refuse(reply, cap, "no_volume");
		goto out;
	}
	memset(v, 0, sizeof *v);
	bind.fd = -1;
	if (p->sys->probe_fresh(p->sys->ctx, bsd, v, &bind) != NTFSMAC_DMN_OK) {
		ntfsmac_priv_release_bind(p, &bind);
		st = ntfsmac_priv_refuse(reply, cap, "probe_failed");
		goto out;
	}

	memset(&in, 0, sizeof in);
	in.probe = v;
	in.mounted = vol.mounted;
	in.peer_authorised = true;
	in.confirmation_ok = ntfsmac_priv_confirmation_ok(v, confirm_label, confirm_serial);
	gate = ntfsmac_policy_gate_repair(&in);
	if (gate != NTFSMAC_GATE_ALLOWED) {
		st = ntfsmac_priv_refuse(reply, cap, ntfsmac_priv_gate_name(gate));
		goto out;
	}
	/*
	 * The entry that path 4 will remove must not belong to a live session (a clone of a volume
	 * that another disk has mounted rw): the same condition as the journal-clear gate.
	 */
	(void)load_entry(p->journal, v, &e, &present);
	if (present && e.session_id != 0 && p->sys->session_live(p->sys->ctx, e.session_id)) {
		st = ntfsmac_priv_refuse(reply, cap, ntfsmac_priv_gate_name(NTFSMAC_GATE_LIVE_SESSION));
		goto out;
	}

	ws = p->sys->clear_dirty(p->sys->ctx, bsd, v, &bind, &vexit);
	bind.fd = -1; /* taken over and closed by the write, whatever it returned */
	if (ws == NTFSMAC_DMN_E_DENIED) {
		st = ntfsmac_priv_refuse(reply, cap, "binding");
		goto out;
	}
	if (ws != NTFSMAC_DMN_OK) {
		st = ntfsmac_priv_failed(reply, cap, ws, "volmark_failed");
		goto out;
	}
	if (present) {
		/* Path 4. A failure leaves the entry: the next attach is ro until journal-clear. */
		removed = ntfsmac_journal_remove_if(p->journal, &e, NTFSMAC_JOURNAL_REMOVE_REPAIR,
						    p->sys->now(p->sys->ctx)) == NTFSMAC_JOURNAL_OK;
	}
	snprintf(reply, cap,
		 "{\"ok\":true,\"result\":\"repaired\",\"journal_entry\":\"%s\",\"warning\":\"%s\"}",
		 !present ? "none" : removed ? "removed" : "kept",
		 present ? "journal_entry" : "none");
	st = NTFSMAC_DMN_OK;
out:
	ntfsmac_priv_release_bind(p, &bind);
	pthread_mutex_unlock(&g_priv_mu);
	return st;
}

/* ------------------------------------------------------------------ */
/* The real facts                                                      */
/* ------------------------------------------------------------------ */

static bool real_volume(void *ctx, const char *bsd, ntfsmac_priv_volume *out)
{
	const ntfsmac_priv_sys_real *r = ctx;

	return r->volume(r->registry_ctx, bsd, out);
}

static bool real_session_live(void *ctx, uint64_t id)
{
	const ntfsmac_priv_sys_real *r = ctx;

	return r->session_live(r->registry_ctx, id);
}

/*
 * Section 4.1 steps 4 and 6 for one request: open A (O_RDONLY), read block 0 and note the
 * DAMediaPath before the probe runs, then run the probe on that same A and keep A open.
 */
static ntfsmac_dmn_status real_probe_fresh(void *ctx, const char *bsd, ntfsmac_probe_verdict *out,
					   ntfsmac_priv_bind *bind)
{
	const ntfsmac_priv_sys_real *r = ctx;
	ntfsmac_probe_run run;
	ntfsmac_disk_info di;
	ntfsmac_dmn_status st;
	int fd;

	bind->fd = -1;
	memset(&di, 0, sizeof di);
	if (r->ss->describe(r->ss->ctx, bsd, &di) != NTFSMAC_DMN_OK)
		return NTFSMAC_DMN_E_IO;
	fd = r->ss->open_device(r->ss->ctx, bsd, false);
	if (fd < 0)
		return NTFSMAC_DMN_E_IO;
	if (r->ss->read_block0(r->ss->ctx, fd, bind->block0) != 0) {
		r->ss->close_fd(r->ss->ctx, fd);
		return NTFSMAC_DMN_E_IO;
	}
	snprintf(bind->media_path, sizeof bind->media_path, "%s", di.media_path);
	memset(&run, 0, sizeof run);
	run.device_fd = fd;
	run.timeout_s = 20;
	st = ntfsmac_spawn_probe(&run, out);
	if (st != NTFSMAC_DMN_OK) {
		r->ss->close_fd(r->ss->ctx, fd);
		return st;
	}
	bind->fd = fd;
	return NTFSMAC_DMN_OK;
}

static void real_bind_release(void *ctx, ntfsmac_priv_bind *bind)
{
	const ntfsmac_priv_sys_real *r = ctx;

	if (bind->fd >= 0)
		r->ss->close_fd(r->ss->ctx, bind->fd);
	bind->fd = -1;
}

/* A bound volmark write on the descriptor in `bind`, which it takes over (clear-repair, sync-mirror). */
static ntfsmac_dmn_status real_bound_write(void *ctx, const char *bsd, const ntfsmac_probe_verdict *v,
					   ntfsmac_priv_bind *bind, ntfsmac_volmark_op op, int *exit_code)
{
	const ntfsmac_priv_sys_real *r = ctx;
	ntfsmac_session_begin_in in;
	ntfsmac_dmn_status st;

	memset(&in, 0, sizeof in);
	in.bsd = bsd;
	in.verdict = v;
	in.probe_fd = bind->fd;
	memcpy(in.block0, bind->block0, sizeof in.block0);
	snprintf(in.media_path, sizeof in.media_path, "%s", bind->media_path);
	st = ntfsmac_session_marker_write(r->ss, &in, op, 20, exit_code);
	bind->fd = -1;
	return st;
}

static ntfsmac_dmn_status real_clear_dirty(void *ctx, const char *bsd, const ntfsmac_probe_verdict *v,
					   ntfsmac_priv_bind *bind, int *exit_code)
{
	return real_bound_write(ctx, bsd, v, bind, NTFSMAC_VOLMARK_CLEAR_REPAIR, exit_code);
}

static ntfsmac_dmn_status real_sync_mirror(void *ctx, const char *bsd, const ntfsmac_probe_verdict *v,
					   ntfsmac_priv_bind *bind, int *exit_code)
{
	return real_bound_write(ctx, bsd, v, bind, NTFSMAC_VOLMARK_SYNC_MIRROR, exit_code);
}

static int64_t real_priv_now(void *ctx)
{
	const ntfsmac_priv_sys_real *r = ctx;

	return r->ss->now(r->ss->ctx);
}

void ntfsmac_priv_sys_real_init(ntfsmac_priv_sys_real *r, const ntfsmac_session_sys *ss,
				bool (*volume)(void *, const char *, ntfsmac_priv_volume *),
				bool (*session_live)(void *, uint64_t), void *registry_ctx)
{
	memset(r, 0, sizeof *r);
	r->ss = ss;
	r->volume = volume;
	r->session_live = session_live;
	r->registry_ctx = registry_ctx;
	r->sys.ctx = r;
	r->sys.volume = real_volume;
	r->sys.probe_fresh = real_probe_fresh;
	r->sys.bind_release = real_bind_release;
	r->sys.session_live = real_session_live;
	r->sys.clear_dirty = real_clear_dirty;
	r->sys.sync_mirror = real_sync_mirror;
	r->sys.now = real_priv_now;
}
