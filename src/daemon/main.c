/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * `ntfsmacd`, the root daemon (T-M4-08). It wires the modules of T-M4-00 to T-M4-07, T-M4-13 and
 * T-M4-14 together: Disk Arbitration (da.c), the probe, volmark and worker (spawn.c), the
 * session lifecycle and journal (session.c), the Unix control socket (ctl.c), the privileged XPC
 * service (ctl_xpc.c), notifications (notify.c) and the mount-table watcher (mountwatch.c).
 * docs/ARCHITECTURE.md sections 3 to 5 and 7 are the specification; this file adds no rule of
 * its own except where it says so.
 *
 * Threads and queues:
 *   - the Disk Arbitration queue (da.c) runs the callbacks below. They are short: they record
 *     the fact, dissent where the attach flow needs it inside the callback (section 4.1 step 3),
 *     and hand the work to the volume's queue.
 *   - one serial queue per volume (BSD name) runs the state machine of section 5: attach (steps 4
 *     to 9), supervision of the worker (a 250 ms timer on the same queue), unmount and teardown
 *     (section 4.5).
 *   - one serial journal queue runs ntfsmac_session_decide(), _begin(), _end(),
 *     _note_write_error() and the privileged requests (journal_clear, repair), so that every
 *     creation and removal of a journal entry for one serial is on one queue (T-M4-14
 *     requirement; the entry removals are compare-and-remove besides).
 *   - the control socket and the XPC listener have threads of their own; list and status read the
 *     registry under one short mutex and never wait for a volume queue.
 *   - the mount-table watcher (H1 of the T-M4-06 review) has a queue of its own: a 2 s timer and a
 *     kqueue VFS event wake it.
 *
 * The registry mutex g_mu is never held across a slow call, a queue hop or a system call that can
 * block.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ctl_priv.h"
#include "mountwatch.h"
#include "ntfsmac/ctl_proto.h"
#include "ntfsmac/log.h"
#include "report.h"
#include "sanitize.h"
#include "spawn_config.h"

#define MAX_VOLS 64u
#define PROBE_TIMEOUT_S 20u
#define MOUNT_WAIT_S 30u
#define TICK_MS 250u
#define REAP_WAIT_MS 5000u
#define FORCE_UNMOUNT_MS 10000u
#define PLAIN_UNMOUNT_MS 6000u /* a plain unmount with its EBUSY retries */
#define INDEXER_WAIT_MS 60000u /* `ntfsmac unmount` refused only because Spotlight or Quick Look holds files: ask again this long (wall clock) */
#define INDEXER_STEP_MS 2000u  /* between two tries; each try is its own job on the volume's queue */
#define UNMOUNT_VERIFY_POLLS 10u   /* after an OK or "not a mount" answer: wait up to 10 x 100 ms for the mount to go */
#define UNMOUNT_VERIFY_STEP_MS 100u
#define UNMOUNTING_STUCK_MS 30000u /* a volume Unmounting for longer, with its mount present, goes back to Mounted */
#define DA_SEEN_WAIT_MS 5000u  /* after a worker exit: wait for Disk Arbitration's unmount report */
#define SHUTDOWN_WAIT_MS 50000u /* all volumes at SIGTERM; the plist's ExitTimeOut is 60 s */
#define REQUEST_WAIT_MS 3000u
#define MW_POLL_S 2u
#define MW_BASELINE_MAX 256u
#define MW_STRAY_PATHS_MAX 8u
#define MW_MOUNTS_MAX 4096u /* the decision handles no more (mountwatch.c) */
#define JOURNAL_PARENT "/var/db/ntfs-macos"

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

/*
 * Only what narrows or locates. There is no option and no file that changes check 4 of ADR-019
 * (env.authz comes from ntfsmac_ctl_default_authz() alone), and nothing here turns a refusal of
 * the policy into a mount.
 */
static struct {
	bool automount_ro;       /* --automount-mode=ro: attaches are read-only (section 3.6) */
#if defined(NTFSMAC_TEST_SEAMS)
	const char *socket_path; /* --socket PATH */
	const char *journal_dir; /* --journal-dir DIR */
	const char *child_log;   /* --child-log FILE: stderr of the probe, volmark and worker */
	bool marker_disabled;    /* --no-marker */
	bool has_console_uid;    /* --console-uid UID: when no one is logged in at the console */
	uid_t console_uid;
#endif
} g_opt;

#if defined(NTFSMAC_TEST_SEAMS)
#define SOCKET_PATH (g_opt.socket_path != NULL ? g_opt.socket_path : NTFSMAC_CTL_SOCKET_PATH)
#define JOURNAL_DIR (g_opt.journal_dir != NULL ? g_opt.journal_dir : NTFSMAC_JOURNAL_DEFAULT_DIR)
#else
#define SOCKET_PATH NTFSMAC_CTL_SOCKET_PATH
#define JOURNAL_DIR NTFSMAC_JOURNAL_DEFAULT_DIR
#endif

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

typedef struct vol vol;

struct vol {
	vol *next;
	char bsd[32];
	dispatch_queue_t q;

	/* Fields marked (m) are protected by g_mu; the rest belongs to the volume's queue. */
	ntfsmac_volume sm;                    /* (m) */
	char label[NTFSMAC_LABEL_PART_SIZE];  /* (m) sanitised */
	char mountpoint[128];                 /* (m) while a mount point exists */
	uint64_t session_id;                  /* (m) 0 without an entry */
	char reason_token[32];                /* (m) why ro, or why Failed */
	ntfsmac_probe_verdict *verdict;       /* (m) the last probe, NULL until the first */
	bool apple_mounted;                   /* (m) some driver has the disk mounted */
	bool dissent;                         /* (m) we hold the dissent for this disk */
	bool wk_live;                         /* (m) a worker process is running */
	uid_t wk_uid;                         /* (m) the user it runs as */
	gid_t wk_gid;                         /* (m) and its group */
	bool stray_pending;                   /* (m) a stray-mount action is queued */
	ntfsmac_unmount_ev uev;               /* (m) this session's evidence of its unmount (rule 3) */

	ntfsmac_mountpoint mp;
	bool mp_valid;
	ntfsmac_session *session;
	ntfsmac_child worker;
	int errrd;
	ntfsmac_errwatch ew;
	bool write_error_noted;
	time_t started;
	dispatch_source_t timer;
	bool want_unmount;     /* a device error asked for an unmount: retry until it works */
	unsigned unmount_tries;
	unsigned tick_count;
	uint64_t unmounting_since; /* monotonic ms when the tick first saw Unmounting; 0 otherwise */
	ntfsmac_umwait um;         /* an `ntfsmac unmount` waits for Spotlight / Quick Look (ADR-028); queue only */
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static vol *g_vols;
static size_t g_nvols;

static ntfsmac_journal *g_journal;
static ntfsmac_da *g_da;
static dispatch_semaphore_t g_da_ready;
static dispatch_queue_t g_journal_q;
static dispatch_queue_t g_mw_q;
static ntfsmac_session_sys_real g_real;
static ntfsmac_session_config g_scfg;
static ntfsmac_priv_sys_real g_priv_real;
static ntfsmac_priv g_priv;
static ntfsmac_conflict_report g_conflict;
static pthread_mutex_t g_conflict_mu = PTHREAD_MUTEX_INITIALIZER;
static ntfsmac_ctl *g_ctl;
static ntfsmac_xpc *g_xpc;
static volatile sig_atomic_t g_stopping;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void sleep_ms(unsigned ms)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

static int64_t now_s(void)
{
	return (int64_t)time(NULL);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
	snprintf(dst, cap, "%s", src != NULL ? src : "");
}

/* The user at the console and their group. false when no one is logged in (root owns the node). */
static bool console_ids(uid_t *uid, gid_t *gid)
{
	struct stat sb;
	struct passwd pw, *res = NULL;
	char buf[1024];
	uid_t u;

#if defined(NTFSMAC_TEST_SEAMS)
	if (g_opt.has_console_uid)
		u = g_opt.console_uid;
	else
#endif
	{
		if (stat("/dev/console", &sb) != 0)
			return false;
		u = sb.st_uid;
	}
	if (u == 0)
		return false;
	if (getpwuid_r(u, &pw, buf, sizeof buf, &res) != 0 || res == NULL)
		return false;
	*uid = u;
	*gid = pw.pw_gid;
	return true;
}

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

/* Caller holds g_mu. */
static vol *vol_find_locked(const char *bsd)
{
	vol *v;

	for (v = g_vols; v != NULL; v = v->next)
		if (strcmp(v->bsd, bsd) == 0)
			return v;
	return NULL;
}

static vol *vol_find(const char *bsd)
{
	vol *v;

	pthread_mutex_lock(&g_mu);
	v = vol_find_locked(bsd);
	pthread_mutex_unlock(&g_mu);
	return v;
}

/*
 * The record of a BSD name lives as long as the daemon: a name that comes back (names are reused)
 * gets the same record, so a queued block never points at freed memory.
 */
static vol *vol_get_or_create(const char *bsd)
{
	vol *v;
	char qname[64];

	pthread_mutex_lock(&g_mu);
	v = vol_find_locked(bsd);
	if (v == NULL && g_nvols < MAX_VOLS) {
		v = calloc(1, sizeof *v);
		if (v != NULL) {
			copy_str(v->bsd, sizeof v->bsd, bsd);
			snprintf(qname, sizeof qname, "org.ntfs-macos.vol.%s", bsd);
			v->q = dispatch_queue_create(qname, DISPATCH_QUEUE_SERIAL);
			if (v->q == NULL) {
				free(v);
				v = NULL;
			} else {
				ntfsmac_volume_init(&v->sm);
				v->sm.state = NTFSMAC_VOL_GONE; /* until the first appearance */
				v->errrd = -1;
				v->worker.pid = -1;
				v->next = g_vols;
				g_vols = v;
				g_nvols++;
			}
		}
	}
	pthread_mutex_unlock(&g_mu);
	return v;
}

static ntfsmac_dmn_status sm_apply(vol *v, ntfsmac_vol_event ev, ntfsmac_vol_mode mode,
				   ntfsmac_vol_reason reason, const char *token)
{
	ntfsmac_vol_event_arg arg;
	ntfsmac_vol_result r;
	ntfsmac_vol_state before;

	arg.mode = mode;
	arg.reason = reason;
	pthread_mutex_lock(&g_mu);
	before = v->sm.state;
	r = ntfsmac_volume_apply(&v->sm, ev, &arg);
	if (r == NTFSMAC_VRES_OK) {
		if (token != NULL)
			copy_str(v->reason_token, sizeof v->reason_token, token);
		else if (v->sm.reason != NTFSMAC_VREASON_NONE)
			copy_str(v->reason_token, sizeof v->reason_token,
				 ntfsmac_report_vreason_token(v->sm.reason));
		else if (ev == NTFSMAC_VEV_START || v->sm.state == NTFSMAC_VOL_APPEARED ||
			 v->sm.state == NTFSMAC_VOL_GONE)
			v->reason_token[0] = '\0';
	}
	pthread_mutex_unlock(&g_mu);
	if (r != NTFSMAC_VRES_OK) {
		NTFSMAC_LOGW("%s: event %s refused in state %s: %s", v->bsd, ntfsmac_vol_event_name(ev),
			     ntfsmac_vol_state_name(before), ntfsmac_vol_result_name(r));
		return r == NTFSMAC_VRES_MODE_MISMATCH ? NTFSMAC_DMN_E_DENIED : NTFSMAC_DMN_E_STATE;
	}
	NTFSMAC_LOGI("%s: %s -> %s", v->bsd, ntfsmac_vol_event_name(ev),
		     ntfsmac_vol_state_name(v->sm.state));
	return NTFSMAC_DMN_OK;
}

static ntfsmac_vol_state vol_state(vol *v)
{
	ntfsmac_vol_state s;

	pthread_mutex_lock(&g_mu);
	s = v->sm.state;
	pthread_mutex_unlock(&g_mu);
	return s;
}

/* ------------------------------------------------------------------ */
/* Journal queue                                                       */
/* ------------------------------------------------------------------ */

typedef struct jq_decide_arg {
	const ntfsmac_probe_verdict *verdict;
	bool automatic, request_ro;
	ntfsmac_session_decision out;
	ntfsmac_dmn_status st;
	bool have_entry;
	ntfsmac_journal_entry entry; /* the entry for the verdict's serial, as it was before the decision */
} jq_decide_arg;

static bool session_live_cb(void *ctx, uint64_t session_id);

static void jq_decide(void *p)
{
	jq_decide_arg *a = p;
	ntfsmac_journal_status js;

	js = ntfsmac_journal_get(g_journal, a->verdict->serial, &a->entry);
	a->have_entry = js == NTFSMAC_JOURNAL_OK;
	a->st = ntfsmac_session_decide(&g_scfg, g_journal, a->verdict, a->automatic, a->request_ro,
				       now_s(), session_live_cb, NULL, &a->out);
}

typedef struct jq_begin_arg {
	const ntfsmac_session_begin_in *in;
	ntfsmac_session *sess;
	ntfsmac_session_start res;
	ntfsmac_dmn_status st;
} jq_begin_arg;

static void jq_begin(void *p)
{
	jq_begin_arg *a = p;

	a->st = ntfsmac_session_begin(&g_real.sys, &g_scfg, g_journal, a->in, &a->sess, &a->res);
}

typedef struct jq_end_arg {
	ntfsmac_session *sess;
	const ntfsmac_errwatch *ew;
	bool unmount_first;
	int exit_status;
	bool note_error_first;
	ntfsmac_session_end_result res;
	ntfsmac_dmn_status st;
} jq_end_arg;

static void jq_end(void *p)
{
	jq_end_arg *a = p;

	if (a->note_error_first)
		(void)ntfsmac_session_note_write_error(a->sess);
	a->st = ntfsmac_session_end(a->sess, a->ew, a->unmount_first, a->exit_status,
				   a->note_error_first, &a->res);
}

static void jq_note_error(void *p)
{
	(void)ntfsmac_session_note_write_error(p);
}

typedef struct jq_peek_arg {
	uint64_t serial;
	bool present;
	ntfsmac_journal_entry e;
} jq_peek_arg;

static void jq_peek(void *p)
{
	jq_peek_arg *a = p;

	a->present = ntfsmac_journal_get(g_journal, a->serial, &a->e) == NTFSMAC_JOURNAL_OK;
}

/* ------------------------------------------------------------------ */
/* Notifications                                                       */
/* ------------------------------------------------------------------ */

static void notify(vol *v, ntfsmac_notify_kind kind, const char *reason)
{
	char label[NTFSMAC_LABEL_PART_SIZE];

	pthread_mutex_lock(&g_mu);
	copy_str(label, sizeof label, v->label);
	pthread_mutex_unlock(&g_mu);
	(void)ntfsmac_notify_post(kind, label, reason);
}

static void notify_token(vol *v, ntfsmac_notify_kind kind, const char *token)
{
	notify(v, kind, token != NULL ? ntfsmac_report_reason_text(token) : NULL);
}

/* ------------------------------------------------------------------ */
/* Mount table                                                         */
/* ------------------------------------------------------------------ */

/* A mount table that cannot be read counts as "gone": the verify-after rule then falls back to trusting the answer. */
static bool mount_present(const char *path)
{
	ntfsmac_mw_mount *m = NULL;
	size_t n = 0, i;
	bool found = false;

	if (ntfsmac_mw_list(&m, &n) != NTFSMAC_DMN_OK)
		return false;
	for (i = 0; i < n; i++)
		if (strcmp(m[i].path, path) == 0 && m[i].fuse) {
			found = true;
			break;
		}
	free(m);
	return found;
}

/*
 * H1 mount-table watcher (mountwatch.h). `g_mw_mu` serialises the checks; the baseline is the set
 * of FUSE mounts that were there before the first worker of their user was started.
 */
static pthread_mutex_t g_mw_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_base[MW_BASELINE_MAX];
static size_t g_nbase;

typedef struct stray_arg {
	vol *v;
	uid_t uid;
	gid_t gid;
	char paths[MW_STRAY_PATHS_MAX][NTFSMAC_MW_PATH_MAX];
	size_t npaths;
	bool found; /* filled in for the volume that asked in mw_check() (`self`), nothing queued */
} stray_arg;

static void stray_run(void *p);

static void mw_prune_baseline(const ntfsmac_mw_mount *m, size_t n)
{
	size_t i, j, keep = 0;

	for (i = 0; i < g_nbase; i++) {
		for (j = 0; j < n; j++)
			if (m[j].id == g_base[i])
				break;
		if (j < n)
			g_base[keep++] = g_base[i];
	}
	g_nbase = keep;
}

/*
 * One look at the table. `uid_to_baseline` != -1: before a worker of that user is started, also
 * record every FUSE mount of that user that is there now (and is not a stray one) as known, so
 * that a mount the user made earlier (sshfs, another macFUSE volume) is never touched. The caller
 * takes this baseline BEFORE the new worker is marked live (review item 2).
 *
 * Returns false when the table could not be read completely (a failure, or more mounts than the
 * decision handles): the check is then failed closed, which means that every live worker is
 * stopped as if it had made a stray mount, and the caller must not start a new worker.
 */
static bool mw_check(long uid_to_baseline, vol *self, stray_arg *self_out)
{
	ntfsmac_mw_mount *m = NULL;
	size_t nm = 0, nw = 0, i, nstray = 0;
	ntfsmac_mw_worker w[MAX_VOLS];
	vol *wv[MAX_VOLS];
	bool *stray = NULL, kill_w[MAX_VOLS];
	bool ok = true;
	vol *v;

	(void)ntfsmac_mw_reap_pending(); /* killed unmount children that were stuck in the kernel */
	pthread_mutex_lock(&g_mw_mu);
	if (ntfsmac_mw_list(&m, &nm) != NTFSMAC_DMN_OK || nm > MW_MOUNTS_MAX) {
		NTFSMAC_LOGE("the mount table could not be read in full (%zu mounts): failing closed", nm);
		ok = false;
		nm = 0;
	}
	stray = calloc(nm > 0 ? nm : 1, sizeof *stray);
	if (stray == NULL) {
		free(m);
		pthread_mutex_unlock(&g_mw_mu);
		return false;
	}
	pthread_mutex_lock(&g_mu);
	for (v = g_vols; v != NULL && nw < MAX_VOLS; v = v->next) {
		if (v->mountpoint[0] == '\0' && !v->wk_live)
			continue;
		memset(&w[nw], 0, sizeof w[nw]);
		w[nw].uid = v->wk_uid;
		copy_str(w[nw].mount_point, sizeof w[nw].mount_point, v->mountpoint);
		w[nw].live = v->wk_live;
		wv[nw++] = v;
	}
	pthread_mutex_unlock(&g_mu);

	mw_prune_baseline(m, nm);
	nstray = ntfsmac_mw_decide(m, nm, w, nw, g_base, g_nbase, stray, kill_w);
	if (!ok) {
		/* Nothing can be judged: stop every live worker, without a mount to remove. */
		for (i = 0; i < nw; i++)
			kill_w[i] = w[i].live;
		nstray = nw;
	}
	for (i = 0; nstray > 0 && i < nw; i++) {
		stray_arg *sa;
		size_t k;
		bool queue = false;

		if (!kill_w[i])
			continue;
		if (self_out != NULL && wv[i] == self) {
			/* The caller is that volume's teardown: it acts itself, nothing is queued. */
			self_out->found = true;
			self_out->uid = w[i].uid;
			for (k = 0; k < nm && self_out->npaths < MW_STRAY_PATHS_MAX; k++)
				if (stray[k] && m[k].owner == w[i].uid) {
					copy_str(self_out->paths[self_out->npaths], sizeof self_out->paths[0],
						 m[k].path);
					self_out->npaths++;
				}
			continue;
		}
		pthread_mutex_lock(&g_mu);
		if (!wv[i]->stray_pending) {
			wv[i]->stray_pending = true;
			queue = true;
		}
		pthread_mutex_unlock(&g_mu);
		if (!queue)
			continue;
		sa = calloc(1, sizeof *sa);
		if (sa == NULL)
			continue;
		sa->v = wv[i];
		sa->uid = w[i].uid;
		pthread_mutex_lock(&g_mu);
		sa->gid = wv[i]->wk_gid;
		pthread_mutex_unlock(&g_mu);
		for (k = 0; k < nm && sa->npaths < MW_STRAY_PATHS_MAX; k++)
			if (stray[k] && m[k].owner == w[i].uid) {
				copy_str(sa->paths[sa->npaths], sizeof sa->paths[0], m[k].path);
				sa->npaths++;
			}
		NTFSMAC_LOGE("%s: a FUSE mount owned by the user of the worker is not a volume of "
			     "this daemon (%zu stray); stopping the worker",
			     wv[i]->bsd, nstray);
		dispatch_async_f(wv[i]->q, sa, stray_run);
	}
	if (ok && uid_to_baseline >= 0) {
		for (i = 0; i < nm; i++) {
			size_t k;
			bool own = false, known = false;

			if (!m[i].fuse || stray[i] || (long)m[i].owner != uid_to_baseline)
				continue;
			for (k = 0; k < nw; k++)
				if (w[k].mount_point[0] != '\0' &&
				    strcmp(w[k].mount_point, m[i].path) == 0)
					own = true;
			for (k = 0; k < g_nbase; k++)
				if (g_base[k] == m[i].id)
					known = true;
			if (own || known)
				continue;
			if (g_nbase >= MW_BASELINE_MAX) {
				NTFSMAC_LOGE("too many FUSE mounts to remember: failing closed");
				ok = false;
				break;
			}
			g_base[g_nbase++] = m[i].id;
		}
	}
	free(stray);
	free(m);
	pthread_mutex_unlock(&g_mw_mu);
	return ok;
}

static void mw_timer_fn(void *p)
{
	(void)p;
	if (!g_stopping)
		(void)mw_check(-1, NULL, NULL);
}

static int g_kq = -1;

static void mw_kq_fn(void *p)
{
	struct kevent ev[4];
	struct timespec zero = {0, 0};

	(void)p;
	while (kevent(g_kq, NULL, 0, ev, 4, &zero) > 0)
		;
	if (!g_stopping)
		(void)mw_check(-1, NULL, NULL);
}

static void mw_start(void)
{
	dispatch_source_t t, s;
	struct kevent kev;

	g_mw_q = dispatch_queue_create("org.ntfs-macos.mountwatch", DISPATCH_QUEUE_SERIAL);
	t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_mw_q);
	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, (int64_t)MW_POLL_S * NSEC_PER_SEC),
				  (uint64_t)MW_POLL_S * NSEC_PER_SEC, NSEC_PER_SEC / 4);
	dispatch_source_set_event_handler_f(t, mw_timer_fn);
	dispatch_resume(t);

	/* A mount or unmount anywhere wakes the watcher at once; the timer is the safety net. */
	g_kq = kqueue();
	if (g_kq >= 0) {
		(void)fcntl(g_kq, F_SETFD, FD_CLOEXEC);
		EV_SET(&kev, 0, EVFILT_FS, EV_ADD | EV_CLEAR, 0, 0, NULL);
		if (kevent(g_kq, &kev, 1, NULL, 0, NULL) == 0) {
			s = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)g_kq, 0, g_mw_q);
			dispatch_source_set_event_handler_f(s, mw_kq_fn);
			dispatch_resume(s);
		} else {
			NTFSMAC_LOGW("kqueue VFS events are not available; polling only");
		}
	}
}

/* ------------------------------------------------------------------ */
/* Worker teardown (section 4.5)                                       */
/* ------------------------------------------------------------------ */

static void timer_stop(vol *v)
{
	if (v->timer != NULL) {
		dispatch_source_cancel(v->timer);
		dispatch_release(v->timer);
		v->timer = NULL;
	}
}

/* Poll until the child is reaped or `ms` pass. */
static bool reap_within(vol *v, unsigned ms, int *exit_status)
{
	unsigned waited = 0;

	for (;;) {
		bool exited = false;
		ntfsmac_dmn_status st;

		if (v->worker.pid <= 1)
			return true;
		st = ntfsmac_child_poll(&v->worker, &exited, exit_status);
		if (st != NTFSMAC_DMN_OK) {
			*exit_status = -1; /* not our child any more: nothing left to wait for */
			v->worker.pid = -1;
			return true;
		}
		if (exited)
			return true;
		if (waited >= ms)
			return false;
		sleep_ms(20);
		waited += 20;
	}
}

typedef struct td_args {
	bool worker_reaped;
	int exit_status;
	bool kill_first;
	bool stray;
	const char (*extra_paths)[NTFSMAC_MW_PATH_MAX]; /* stray mounts to force-unmount, after the kill */
	size_t n_extra;
} td_args;

static ntfsmac_notify_kind report_kind(ntfsmac_session_report r, bool *post)
{
	*post = true;
	switch (r) {
	case NTFSMAC_SREPORT_CLEAN:
		return NTFSMAC_NOTIFY_EJECTED_CLEAN;
	case NTFSMAC_SREPORT_CLEAN_NO_FLUSH:
		return NTFSMAC_NOTIFY_EJECTED_NO_FLUSH_CONFIRM;
	case NTFSMAC_SREPORT_UNCLEAN:
	case NTFSMAC_SREPORT_CLEAR_FAILED:
		return NTFSMAC_NOTIFY_SESSION_UNCLEAN;
	case NTFSMAC_SREPORT_RO_CLOSED:
	default:
		*post = false;
		return NTFSMAC_NOTIFY_SESSION_UNCLEAN;
	}
}

/*
 * Rule 3 of the clean-session proof (section 7): the unmount was seen before the worker ended.
 * Two things count, nothing else: the daemon's own successful (or still running) unmount call,
 * and the Disk Arbitration report that macFUSE's disk of this volume's mount disappeared. A mount
 * that is merely gone does not count: killing the FSKit module process ends the mount without
 * either, and what the kernel still held for the worker is lost (T-M5-05).
 */
static void note_unmount_call(vol *v, ntfsmac_dmn_status st)
{
	/* Only OK counts, and only when no mount is left: a timeout can also mean that every
	 * unmount(2) got EBUSY, and the mount is then still there. */
	bool still = v->mp_valid && mount_present(v->mp.path);

	pthread_mutex_lock(&g_mu);
	ntfsmac_uev_note_unmount_call(&v->uev, st, still);
	pthread_mutex_unlock(&g_mu);
}

static void note_unmount_failed(vol *v)
{
	pthread_mutex_lock(&g_mu);
	ntfsmac_uev_unmount_failed(&v->uev);
	pthread_mutex_unlock(&g_mu);
}

static uint64_t ext_now_ms(void);

/*
 * One bounded unmount with the verify-after rule: an OK or "not a mount" answer counts only when
 * the mount table agrees (found with a mount that outlived an EINVAL answer). Logs the request,
 * the child's result with the real errno, the duration and the mount's presence afterwards.
 * `src` says who asked: cli, device-error or shutdown. The caller has checked v->mp_valid.
 */
static ntfsmac_dmn_status unmount_checked(vol *v, const char *src, bool force, unsigned retries,
					  unsigned timeout_ms, pid_t *busy)
{
	ntfsmac_mw_um_info info;
	ntfsmac_dmn_status st, answer;
	bool still;
	unsigned i;

	NTFSMAC_LOGI("%s: unmount requested by %s (%s)", v->bsd, src, force ? "force" : "plain");
	st = ntfsmac_mw_unmount_bounded_ex(v->mp.path, force, retries, timeout_ms, busy, &info);
	answer = st;
	still = mount_present(v->mp.path);
	for (i = 0; still && (st == NTFSMAC_DMN_OK || st == NTFSMAC_DMN_E_STATE) && i < UNMOUNT_VERIFY_POLLS; i++) {
		sleep_ms(UNMOUNT_VERIFY_STEP_MS);
		still = mount_present(v->mp.path);
	}
	st = ntfsmac_unmount_verify(st, still);
	NTFSMAC_LOGI("%s: unmount by %s: status %s, child exit %d, errno %d, %u ms, mount %s afterwards", v->bsd,
		     src, ntfsmac_dmn_status_name(answer), info.child_code, info.err, info.ms,
		     still ? "present" : "gone");
	if (st != answer)
		NTFSMAC_LOGW("%s: unmount answered %s but %s is still mounted", v->bsd,
			     ntfsmac_dmn_status_name(answer), v->mp.path);
	/* Evidence (rule 3) takes the raw answer: "not a mount any more" with the mount gone is a mount
	 * that merely vanished and must not count as our unmount (T-M5-05). Only a real OK does. */
	note_unmount_call(v, answer == NTFSMAC_DMN_OK ? st : answer);
	return st;
}

static bool unmount_evidence(vol *v, bool *requested, bool *da_seen)
{
	ntfsmac_unmount_ev e;

	pthread_mutex_lock(&g_mu);
	e = v->uev;
	pthread_mutex_unlock(&g_mu);
	if (requested != NULL)
		*requested = e.requested;
	if (da_seen != NULL)
		*da_seen = e.da_seen;
	return ntfsmac_uev_have(&e);
}

/*
 * The worker has ended after a plain unmount that the daemon did not make: the DA report may
 * still be on its way (it can arrive after the worker's exit). Wait for it, bounded; a module
 * death never produces it, so that case ends here after the bound with the session not clean.
 */
static void wait_unmount_evidence(vol *v)
{
	unsigned waited = 0;

	for (;;) {
		ntfsmac_unmount_ev e;

		pthread_mutex_lock(&g_mu);
		e = v->uev;
		pthread_mutex_unlock(&g_mu);
		if (!ntfsmac_uev_wait_more(&e, waited, DA_SEEN_WAIT_MS, g_stopping != 0))
			break;
		sleep_ms(50);
		waited += 50;
	}
}

/*
 * After the worker is gone (or is made to go): section 4.5 steps 2 to 4 and the H1 steps. On the
 * volume's queue. The volume must be Mounted or Unmounting.
 */
static void vol_teardown(vol *v, const td_args *a)
{
	int exit_status = a->exit_status;
	bool unmount_first, still_mounted = false, session_ok = true;
	unsigned killed = 0;
	bool device_held = false;
	ntfsmac_dmn_status st;
	ntfsmac_vol_state state = vol_state(v);
	size_t i;
	stray_arg late;
	bool stray = a->stray;
	const char (*extra)[NTFSMAC_MW_PATH_MAX] = a->extra_paths;
	size_t n_extra = a->n_extra;

	memset(&late, 0, sizeof late);
	if (state != NTFSMAC_VOL_MOUNTED && state != NTFSMAC_VOL_UNMOUNTING)
		return;
	if (state == NTFSMAC_VOL_MOUNTED)
		(void)sm_apply(v, NTFSMAC_VEV_UNMOUNT, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_NONE, NULL);
	timer_stop(v);

	/* 1. The worker and its whole tree, first: a hijacked one must not mount again. */
	if (!a->worker_reaped && v->worker.pid > 1) {
		if (a->kill_first)
			(void)ntfsmac_child_kill_group(&v->worker);
		if (!reap_within(v, REAP_WAIT_MS, &exit_status)) {
			NTFSMAC_LOGE("%s: the worker did not die after SIGKILL; going on without it", v->bsd);
			exit_status = -1;
		}
	}
	/*
	 * A worker that mounted something and then ended before the next look at the table would
	 * escape: look once more while it still counts as live (review item 5). A stray found here
	 * is handled below like one that stray_run() was given.
	 */
	if (!g_stopping)
		(void)mw_check(-1, v, &late);
	if (late.found && !a->stray && a->n_extra == 0) {
		stray = true;
		extra = (const char (*)[NTFSMAC_MW_PATH_MAX])late.paths;
		n_extra = late.npaths;
		NTFSMAC_LOGE("%s: the worker ended, and a FUSE mount of its user is not a volume of this daemon", v->bsd);
	}
	pthread_mutex_lock(&g_mu);
	v->wk_live = false;
	pthread_mutex_unlock(&g_mu);

	/*
	 * 2. The extra mounts of a hijacked worker, with a time bound. The path is the user's to
	 * change, so root never unmounts it: the unmount is made by a child that has become the user
	 * (the kernel lets a user unmount only mounts of their own). Review item 3.
	 */
	for (i = 0; i < n_extra; i++) {
		st = ntfsmac_mw_unmount_as_user(extra[i], v->wk_uid, v->wk_gid, FORCE_UNMOUNT_MS);
		NTFSMAC_LOGW("%s: forced unmount of a stray mount (as the user): %s", v->bsd,
			     ntfsmac_dmn_status_name(st));
	}

	/* 3. The channel, to its end. */
	if (v->errrd >= 0) {
		st = ntfsmac_errwatch_drain(&v->ew, v->errrd, NTFSMAC_ERRWATCH_EOF_WAIT_MS);
		if (st != NTFSMAC_DMN_OK)
			NTFSMAC_LOGW("%s: the session channel did not end: %s", v->bsd,
				     ntfsmac_dmn_status_name(st));
	}

	/*
	 * 4. The mount. "Unmount seen before the exit" is read from the table now: a worker that
	 * ended after FUSE_DESTROY leaves no mount; one that crashed leaves it, and it is then
	 * removed by force (the worker is gone, section 4.5 step 1).
	 */
	if (v->mp_valid) {
		bool requested = false, da_seen = false;

		still_mounted = mount_present(v->mp.path);
		/* Only a worker that ended by itself with exit 0 can still make a clean session. */
		if (!still_mounted && exit_status == 0 && !stray && !a->kill_first)
			wait_unmount_evidence(v);
		(void)unmount_evidence(v, &requested, &da_seen);
		unmount_first = ntfsmac_unmount_seen(!still_mounted, requested, da_seen);
		if (!still_mounted && !unmount_first)
			NTFSMAC_LOGW("%s: the mount ended with no unmount request of ours and no Disk "
				     "Arbitration report of one (the macFUSE module died?): the session is "
				     "not clean", v->bsd);
		if (still_mounted) {
			/*
			 * The session's device descriptor (s->fd) stays open here: the journal step below
			 * still needs it to write the entry and the marker. The unmount child does not
			 * inherit it (run_bounded closes every descriptor >= 3 in the child).
			 */
			st = ntfsmac_mw_force_unmount(v->mp.path, FORCE_UNMOUNT_MS);
			sleep_ms(100);
			still_mounted = mount_present(v->mp.path);
			NTFSMAC_LOGW("%s: forced unmount of the volume: %s%s", v->bsd,
				     ntfsmac_dmn_status_name(st), still_mounted ? " (still mounted)" : "");
		}
	} else {
		unmount_first = true;
	}

	/* 5. Nothing of the worker's user may still hold the device. */
	st = ntfsmac_child_device_check(&v->worker, &killed);
	device_held = st != NTFSMAC_DMN_OK && st != NTFSMAC_DMN_E_STATE;
	if (device_held)
		NTFSMAC_LOGE("%s: a process still holds the device (%s): the session is not clean", v->bsd,
			     ntfsmac_dmn_status_name(st));
	if (killed > 0)
		NTFSMAC_LOGW("%s: %u process(es) that held the device were killed", v->bsd, killed);

	/* 6. The session: journal, marker, descriptor (section 4.5 step 3). */
	if (v->session != NULL) {
		jq_end_arg ea;

		memset(&ea, 0, sizeof ea);
		ea.sess = v->session;
		ea.ew = &v->ew;
		ea.unmount_first = unmount_first;
		ea.exit_status = exit_status;
		/* "mark the session failed in the journal": a stray mount, or a holder of the device that
		 * could not be cleared, so the entry and the marker stay and the next attach is ro. */
		ea.note_error_first = stray || device_held;
		dispatch_sync_f(g_journal_q, &ea, jq_end);
		session_ok = ea.st == NTFSMAC_DMN_OK;
		if (!session_ok)
			NTFSMAC_LOGE("%s: the session could not be settled (%s)", v->bsd,
				     ntfsmac_dmn_status_name(ea.st));
		if (stray) {
			notify(v, NTFSMAC_NOTIFY_STRAY_MOUNT, NULL);
		} else if (session_ok) {
			bool post;
			ntfsmac_notify_kind k = report_kind(ea.res.report, &post);

			if (ea.res.clear_interrupted)
				notify(v, NTFSMAC_NOTIFY_VOLMARK_INTERRUPTED,
				       "The dirty marker could not be cleared. If the volume does not "
				       "mount, run ntfsmac repair --sync-mirror or Windows chkdsk.");
			else if (post && k == NTFSMAC_NOTIFY_SESSION_UNCLEAN) {
				if (device_held)
					notify(v, k, "A program still had the drive open; see the log.");
				else if (!v->write_error_noted)
					notify(v, k, ea.res.report == NTFSMAC_SREPORT_CLEAR_FAILED
							     ? "The dirty marker could not be cleared."
							     : NULL);
			} else if (post)
				notify(v, k, NULL);
		}
		ntfsmac_session_free(v->session);
		v->session = NULL;
		pthread_mutex_lock(&g_mu);
		v->session_id = 0;
		pthread_mutex_unlock(&g_mu);
	}

	/* 7. The mount point directory, only if the mount is gone; the descriptors. */
	if (v->mp_valid) {
		if (!still_mounted) {
			st = ntfsmac_mountpoint_remove(&v->mp);
			if (st != NTFSMAC_DMN_OK)
				NTFSMAC_LOGW("%s: the mount point directory was left: %s", v->bsd,
					     ntfsmac_dmn_status_name(st));
		}
		v->mp_valid = false;
		pthread_mutex_lock(&g_mu);
		v->mountpoint[0] = '\0';
		pthread_mutex_unlock(&g_mu);
	}
	if (v->errrd >= 0) {
		close(v->errrd);
		v->errrd = -1;
	}
	v->worker.pid = -1;
	v->want_unmount = false;
	pthread_mutex_lock(&g_mu);
	ntfsmac_uev_reset(&v->uev);
	pthread_mutex_unlock(&g_mu);

	/* 8. Report the volume detached. */
	if (stray)
		(void)sm_apply(v, NTFSMAC_VEV_UNMOUNT_FAILED, NTFSMAC_VMODE_NONE,
			       NTFSMAC_VREASON_STRAY_MOUNT, NULL);
	else if (still_mounted)
		(void)sm_apply(v, NTFSMAC_VEV_UNMOUNT_FAILED, NTFSMAC_VMODE_NONE,
			       NTFSMAC_VREASON_UNMOUNT, NULL);
	else
		(void)sm_apply(v, NTFSMAC_VEV_UNMOUNTED, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_NONE, NULL);
}

static void stray_run(void *p)
{
	stray_arg *sa = p;
	vol *v = sa->v;
	td_args a;
	ntfsmac_vol_state s = vol_state(v);

	pthread_mutex_lock(&g_mu);
	v->stray_pending = false;
	pthread_mutex_unlock(&g_mu);
	if (s == NTFSMAC_VOL_MOUNTED || s == NTFSMAC_VOL_UNMOUNTING) {
		memset(&a, 0, sizeof a);
		a.kill_first = true;
		a.stray = true;
		a.extra_paths = (const char (*)[NTFSMAC_MW_PATH_MAX])sa->paths;
		a.n_extra = sa->npaths;
		vol_teardown(v, &a);
	}
	free(sa);
}

/* One plain unmount request after a device error; later a forced one. */
static void unmount_after_error(vol *v)
{
	pid_t busy = 0;
	ntfsmac_dmn_status st;
	bool force = v->unmount_tries >= 5u; /* about 10 s of plain attempts, then the volume is failing anyway */

	if (!v->mp_valid)
		return;
	v->unmount_tries++;
	/* Every unmount has a time bound: a stopped worker can hold unmount(2) for minutes. */
	st = unmount_checked(v, "device-error", force, force ? 0u : 1u,
			     force ? FORCE_UNMOUNT_MS : PLAIN_UNMOUNT_MS, &busy);
	/* Only a mount that is really gone ends the retries. */
	if (st == NTFSMAC_DMN_OK)
		v->want_unmount = false;
}

/* The 250 ms supervision of a mounted volume's worker. */
static void vol_tick(void *p)
{
	vol *v = p;
	bool exited = false;
	int es = -1;
	ntfsmac_dmn_status st;
	ntfsmac_vol_state s = vol_state(v);

	if (v->timer == NULL || (s != NTFSMAC_VOL_MOUNTED && s != NTFSMAC_VOL_UNMOUNTING))
		return;
	v->tick_count++;
	if (v->errrd >= 0)
		(void)ntfsmac_errwatch_pump(&v->ew, v->errrd);
	/* First `E` record or a broken channel (section 7). */
	if (!v->write_error_noted && v->session != NULL &&
	    (v->ew.flags & (NTFSMAC_EW_SAW_E | NTFSMAC_EW_PROTOCOL_ERROR)) != 0u) {
		char when[16];
		struct tm tm;

		v->write_error_noted = true;
		dispatch_sync_f(g_journal_q, v->session, jq_note_error);
		if (localtime_r(&v->started, &tm) == NULL || strftime(when, sizeof when, "%H:%M", &tm) == 0)
			when[0] = '\0';
		notify(v, NTFSMAC_NOTIFY_WRITE_ERROR, when[0] != '\0' ? when : NULL);
		v->want_unmount = true;
		v->unmount_tries = 0;
	}
	if (v->worker.pid > 1) {
		st = ntfsmac_child_poll(&v->worker, &exited, &es);
		if (st != NTFSMAC_DMN_OK) {
			exited = true;
			es = -1;
			v->worker.pid = -1;
		}
		if (exited) {
			td_args a;

			memset(&a, 0, sizeof a);
			a.worker_reaped = true;
			a.exit_status = es;
			vol_teardown(v, &a);
			return;
		}
	}
	/* Safety net: Unmounting only ends when the worker does. A mount that stays after an answer we
	 * trusted must not hold the volume there for ever. */
	if (s == NTFSMAC_VOL_UNMOUNTING && v->unmounting_since == 0u) {
		v->unmounting_since = ext_now_ms();
	} else if (s != NTFSMAC_VOL_UNMOUNTING) {
		v->unmounting_since = 0u;
	} else if (v->um.waiting) {
		/* An indexer wait is running (ADR-028, up to INDEXER_WAIT_MS, longer than the 30 s bound): it ends
		 * itself with UNMOUNT_BUSY, so the stuck net is suspended meanwhile and starts over after it. */
		v->unmounting_since = ext_now_ms();
	} else if (ntfsmac_unmount_stuck(v->unmounting_since, ext_now_ms(), UNMOUNTING_STUCK_MS)) {
		bool present = v->mp_valid && mount_present(v->mp.path);

		NTFSMAC_LOGW("%s: Unmounting for over %u s with the worker alive and the mount %s", v->bsd,
			     UNMOUNTING_STUCK_MS / 1000u, present ? "present: back to Mounted" : "gone");
		v->unmounting_since = ext_now_ms(); /* the next look is one bound later */
		if (present) {
			note_unmount_failed(v);
			(void)sm_apply(v, NTFSMAC_VEV_UNMOUNT_BUSY, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_NONE, NULL);
			return;
		}
	}
	if (v->want_unmount && (v->tick_count % 8u) == 0u)
		unmount_after_error(v);
}

static void timer_start(vol *v)
{
	v->tick_count = 0;
	v->unmounting_since = 0u;
	v->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, v->q);
	if (v->timer == NULL)
		return;
	dispatch_source_set_timer(v->timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)TICK_MS * NSEC_PER_MSEC),
				  (uint64_t)TICK_MS * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
	dispatch_set_context(v->timer, v);
	dispatch_source_set_event_handler_f(v->timer, vol_tick);
	dispatch_resume(v->timer);
}

/* ------------------------------------------------------------------ */
/* Attach (section 4.1)                                                */
/* ------------------------------------------------------------------ */

typedef struct attach_req {
	vol *v;
	bool automatic;
	bool request_ro;
	dispatch_semaphore_t done; /* signalled at the end; may be NULL. Retained by the sender. */
} attach_req;

static void attach_done(attach_req *rq)
{
	if (rq->done != NULL) {
		dispatch_semaphore_signal(rq->done);
		dispatch_release(rq->done);
	}
	free(rq);
}

/* Worker failed to start or to mount: settle what exists, in the order of section 4.5. */
static void settle_failed_spawn(vol *v, int rd, int wr)
{
	unsigned killed = 0;
	jq_end_arg ea;
	ntfsmac_errwatch ew;

	/* v->worker holds the handle; its pid is above 1 only if the process was not reaped yet. */
	if (v->worker.pid > 1) {
		int es;

		(void)ntfsmac_child_kill_group(&v->worker);
		(void)reap_within(v, REAP_WAIT_MS, &es);
	}
	if (v->mp_valid && mount_present(v->mp.path))
		(void)ntfsmac_mw_force_unmount(v->mp.path, FORCE_UNMOUNT_MS);
	/* A holder that remains changes nothing below: this session never gets a proof of a clean end. */
	(void)ntfsmac_child_device_check(&v->worker, &killed);
	if (wr >= 0)
		close(wr);
	if (rd >= 0)
		close(rd);
	/* No proof of a clean session exists: the entry and the marker stay (section 7). */
	ntfsmac_errwatch_init(&ew);
	memset(&ea, 0, sizeof ea);
	ea.sess = v->session;
	ea.ew = &ew;
	ea.exit_status = -1;
	dispatch_sync_f(g_journal_q, &ea, jq_end);
	ntfsmac_session_free(v->session);
	v->session = NULL;
	pthread_mutex_lock(&g_mu);
	v->session_id = 0;
	v->wk_live = false;
	v->mountpoint[0] = '\0';
	pthread_mutex_unlock(&g_mu);
	v->worker.pid = -1;
	if (v->mp_valid) {
		if (!mount_present(v->mp.path))
			(void)ntfsmac_mountpoint_remove(&v->mp);
		v->mp_valid = false;
	}
}

#define WORKER_RETRY_DELAY_MS 1500u

static void log_worker_exit(const vol *v, const ntfsmac_child *child)
{
	char how[48];

	ntfsmac_child_exit_text(child, how, sizeof how);
	NTFSMAC_LOGW("%s: the worker %s before the mount appeared", v->bsd, how);
}

/*
 * The one retry of the worker start (section 4.1 step 8). Every fact below must hold, or the
 * failure is settled as it always was: the start chain exited on its own with the worker's exit
 * status 21 (ntfsmac_spawn_retry_wanted: never a timeout, never a refused vector), the session
 * channel carried no S record, nothing is mounted at the mount point, no process of the user
 * holds the device, and the mount table shows no stray mount of this worker's user.
 */
static bool worker_retry_ok(vol *v, ntfsmac_dmn_status st, ntfsmac_child *child, int rd)
{
	unsigned killed = 0;
	stray_arg sa;

	if (!ntfsmac_spawn_retry_wanted(st, child))
		return false;
	if (g_stopping)
		return false;
	/*
	 * The retry starts a new record stream, so nothing of the first one may be lost: a rw
	 * ntfs-3g can emit E or W records while it unwinds a failed fuse_mount. Any flag (S, E, W, U,
	 * a protocol error; EOF alone is tolerated, it follows every record already parsed) or a
	 * failed read fails closed.
	 */
	if (ntfsmac_errwatch_pump(&v->ew, rd) != NTFSMAC_DMN_OK)
		return false;
	if ((v->ew.flags & ~(uint32_t)NTFSMAC_EW_EOF) != 0)
		return false;
	if (v->mp_valid && mount_present(v->mp.path))
		return false;
	/* A leftover that had to be killed may also hold the write end of the channel. */
	if (ntfsmac_child_device_check(child, &killed) != NTFSMAC_DMN_OK || killed != 0)
		return false;
	memset(&sa, 0, sizeof sa);
	if (!mw_check(-1, v, &sa))
		return false;
	if (sa.found) {
		size_t i;

		for (i = 0; i < sa.npaths; i++)
			NTFSMAC_LOGW("%s: no retry: stray mount %s", v->bsd, sa.paths[i]);
		return false;
	}
	return true;
}

static const char *refuse_reason_for(const ntfsmac_probe_verdict *pv, ntfsmac_policy_note note)
{
	(void)pv;
	return ntfsmac_report_reason_text(ntfsmac_report_policy_note_token(note));
}

/* The notification of a refused volume: the interrupted-marker text when the journal explains it. */
static void notify_refused(vol *v, const ntfsmac_probe_verdict *pv, ntfsmac_policy_note note,
			   bool have_entry, const ntfsmac_journal_entry *e)
{
	bool interrupted = have_entry && e != NULL &&
			   (e->marker == NTFSMAC_JOURNAL_MARKER_PENDING ||
			    (e->marker == NTFSMAC_JOURNAL_MARKER_SET &&
			     e->state == NTFSMAC_JOURNAL_STATE_OPEN));

	if (!pv->is_ntfs)
		return; /* a disk that is not NTFS is none of our business: no noise */
	if (note == NTFSMAC_POLICY_NOTE_MIRROR_SPLIT) {
		if (interrupted)
			notify(v, NTFSMAC_NOTIFY_VOLMARK_INTERRUPTED,
			       "Run ntfsmac repair --sync-mirror, or Windows chkdsk.");
		else
			notify(v, NTFSMAC_NOTIFY_MIRROR_SPLIT, NULL);
		return;
	}
	if (interrupted && pv->state == NTFSMAC_PROBE_STATE_CORRUPT) {
		notify(v, NTFSMAC_NOTIFY_VOLMARK_INTERRUPTED, "Run Windows chkdsk.");
		return;
	}
	notify(v, NTFSMAC_NOTIFY_REFUSED, refuse_reason_for(pv, note));
}

static void attach_run(void *arg)
{
	attach_req *rq = arg;
	vol *v = rq->v;
	ntfsmac_probe_verdict *pv = NULL;
	ntfsmac_session_begin_in *in = NULL;
	ntfsmac_disk_info info;
	ntfsmac_probe_run pr;
	ntfsmac_dmn_status st;
	jq_decide_arg da;
	jq_begin_arg ba;
	ntfsmac_worker_run wrun;
	ntfsmac_child child;
	ntfsmac_mountpoint mp;
	uid_t cuid = 0;
	gid_t cgid = 0;
	int fd = -1, rd = -1, wr = -1;
	bool mounted_ro = false, ro_final;
	ntfsmac_vol_mode mode;
	char ro_token[32] = "";
	ntfsmac_policy_decision dec;
	ntfsmac_vol_result vr;
	ntfsmac_vol_reason why;
	bool accepted;
	char label[NTFSMAC_LABEL_PART_SIZE];

	memset(&ba, 0, sizeof ba);
	pthread_mutex_lock(&g_mu);
	accepted = ntfsmac_volume_accepts_mount(&v->sm);
	pthread_mutex_unlock(&g_mu);
	if (!accepted) {
		NTFSMAC_LOGW("%s: attach ignored in state %s", v->bsd, ntfsmac_vol_state_name(vol_state(v)));
		goto out;
	}
	if (sm_apply(v, NTFSMAC_VEV_START, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_NONE, NULL) != NTFSMAC_DMN_OK)
		goto out;
	pv = calloc(1, sizeof *pv);
	in = calloc(1, sizeof *in);
	if (pv == NULL || in == NULL) {
		(void)sm_apply(v, NTFSMAC_VEV_PROBE_FAILED, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_PROBE_FAILED, NULL);
		goto out;
	}
	if (!console_ids(&cuid, &cgid)) {
		NTFSMAC_LOGW("%s: no user at the console: the worker cannot run, leaving the disk to the system", v->bsd);
		(void)ntfsmac_da_withdraw(g_da, v->bsd);
		(void)sm_apply(v, NTFSMAC_VEV_PROBE_FAILED, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_PROBE_FAILED, NULL);
		goto out;
	}

	/* Step 3: Apple's driver mounted the disk first: unmount it, then nothing holds the device. */
	pthread_mutex_lock(&g_mu);
	if (v->apple_mounted) {
		pthread_mutex_unlock(&g_mu);
		st = ntfsmac_da_unmount(g_da, v->bsd);
		if (st != NTFSMAC_DMN_OK && st != NTFSMAC_DMN_E_STATE) {
			NTFSMAC_LOGW("%s: Apple's mount could not be removed: %s", v->bsd, ntfsmac_dmn_status_name(st));
			(void)sm_apply(v, NTFSMAC_VEV_PROBE_FAILED, NTFSMAC_VMODE_NONE,
				       NTFSMAC_VREASON_PROBE_FAILED, "other_driver");
			goto out;
		}
		pthread_mutex_lock(&g_mu);
		v->apple_mounted = false;
	}
	pthread_mutex_unlock(&g_mu);

	/* Step 4: descriptor A, the first 4096 bytes, the disk's identity, the probe. */
	fd = g_real.sys.open_device(g_real.sys.ctx, v->bsd, false);
	if (fd < 0 || g_real.sys.read_block0(g_real.sys.ctx, fd, in->block0) != 0 ||
	    ntfsmac_da_describe(g_da, v->bsd, &info) != NTFSMAC_DMN_OK) {
		NTFSMAC_LOGW("%s: the device could not be opened or described (errno %d)", v->bsd, errno);
		if (fd >= 0)
			g_real.sys.close_fd(g_real.sys.ctx, fd);
		(void)sm_apply(v, NTFSMAC_VEV_PROBE_FAILED, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_PROBE_FAILED, NULL);
		goto out;
	}
	copy_str(in->media_path, sizeof in->media_path, info.media_path);
	pr.device_fd = fd;
	pr.timeout_s = PROBE_TIMEOUT_S;
	st = ntfsmac_spawn_probe(&pr, pv); /* any failure leaves the fail-closed `unknown` verdict */
	if (st != NTFSMAC_DMN_OK)
		NTFSMAC_LOGW("%s: the probe did not give a verdict (%s); treating it as unknown", v->bsd,
			     ntfsmac_dmn_status_name(st));
	(void)sm_apply(v, NTFSMAC_VEV_PROBE_DONE, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_NONE, NULL);

	if (pv->is_ntfs)
		(void)ntfsmac_label_sanitize(pv->label, pv->label_len, pv->serial, label, sizeof label);
	else
		label[0] = '\0';
	pthread_mutex_lock(&g_mu);
	if (v->verdict == NULL)
		v->verdict = calloc(1, sizeof *v->verdict);
	if (v->verdict != NULL)
		memcpy(v->verdict, pv, sizeof *pv);
	copy_str(v->label, sizeof v->label, label);
	pthread_mutex_unlock(&g_mu);

	/* Step 5: the policy, with the journal (on the journal queue). */
	memset(&da, 0, sizeof da);
	da.verdict = pv;
	da.automatic = rq->automatic;
	da.request_ro = rq->request_ro;
	dispatch_sync_f(g_journal_q, &da, jq_decide);
	if (da.st != NTFSMAC_DMN_OK) {
		NTFSMAC_LOGE("%s: the policy decision failed (%s)", v->bsd, ntfsmac_dmn_status_name(da.st));
		g_real.sys.close_fd(g_real.sys.ctx, fd);
		(void)sm_apply(v, NTFSMAC_VEV_REFUSED, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_POLICY_REFUSED, NULL);
		goto out;
	}
	dec = da.out.policy.decision;
	NTFSMAC_LOGI("%s: probe state %s, decision %s", v->bsd, ntfsmac_probe_state_name(pv->state),
		     ntfsmac_policy_decision_name(dec));
	if (dec == NTFSMAC_POLICY_REFUSE) {
		g_real.sys.close_fd(g_real.sys.ctx, fd);
		(void)ntfsmac_da_withdraw(g_da, v->bsd); /* the system handles the disk (step 5) */
		pthread_mutex_lock(&g_mu);
		v->dissent = false;
		pthread_mutex_unlock(&g_mu);
		(void)sm_apply(v, NTFSMAC_VEV_REFUSED, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_POLICY_REFUSED,
			       ntfsmac_report_policy_note_token(da.out.policy.note));
		notify_refused(v, pv, da.out.policy.note, da.have_entry, &da.entry);
		goto out;
	}
	copy_str(ro_token, sizeof ro_token, ntfsmac_report_policy_note_token(da.out.policy.note));
	if (da.out.evidence_removed)
		copy_str(ro_token, sizeof ro_token, "external_check");
	if (ntfsmac_session_serial_token(&da.out) != NULL)
		copy_str(ro_token, sizeof ro_token, ntfsmac_session_serial_token(&da.out));

	/*
	 * H1 baseline: the FUSE mounts that this user has now are known and never touched. It is taken
	 * before the worker is marked live (below), or every mount of the user would look new. A table
	 * that cannot be read in full refuses the mount (fail closed).
	 */
	if (!mw_check((long)cuid, NULL, NULL)) {
		NTFSMAC_LOGE("%s: the mount table could not be checked; the volume is not mounted", v->bsd);
		g_real.sys.close_fd(g_real.sys.ctx, fd);
		(void)sm_apply(v, NTFSMAC_VEV_REFUSED, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_SPAWN, NULL);
		notify_token(v, NTFSMAC_NOTIFY_REFUSED, "spawn");
		goto out;
	}

	/*
	 * The mount point (step 7) is made before the session starts, so that a failure here cannot
	 * leave a dirty marker behind. Nothing names the path before the mount.
	 */
	memset(&mp, 0, sizeof mp);
	mp.dirfd = -1;
	st = ntfsmac_mountpoint_create(pv->label, pv->label_len, pv->serial, cuid, cgid, &mp);
	if (st != NTFSMAC_DMN_OK) {
		NTFSMAC_LOGW("%s: no mount point (%s)", v->bsd, ntfsmac_dmn_status_name(st));
		g_real.sys.close_fd(g_real.sys.ctx, fd);
		(void)sm_apply(v, NTFSMAC_VEV_REFUSED, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_MOUNT_POINT, NULL);
		notify(v, NTFSMAC_NOTIFY_REFUSED, ntfsmac_report_reason_text("mount_point"));
		goto out;
	}

	/* Step 6: binding, journal entry, volmark (the session module). It owns the descriptor now. */
	in->bsd = v->bsd;
	in->verdict = pv;
	in->decision = dec;
	in->probe_fd = fd;
	ba.in = in;
	dispatch_sync_f(g_journal_q, &ba, jq_begin);
	fd = -1; /* NOLINT(clang-analyzer-deadcode.DeadStores): ownership marker, the session module owns it now */
	if (ba.st != NTFSMAC_DMN_OK) {
		NTFSMAC_LOGW("%s: the session was refused (%s, %s)", v->bsd, ntfsmac_dmn_status_name(ba.st),
			     ntfsmac_report_vreason_token(ba.res.failed));
		(void)ntfsmac_mountpoint_remove(&mp);
		why = ba.res.failed != NTFSMAC_VREASON_NONE ? ba.res.failed : NTFSMAC_VREASON_BINDING;
		(void)sm_apply(v, NTFSMAC_VEV_REFUSED, NTFSMAC_VMODE_NONE, why, NULL);
		notify_token(v, NTFSMAC_NOTIFY_REFUSED, ntfsmac_report_vreason_token(why));
		goto out;
	}
	v->session = ba.sess;
	v->mp = mp;
	v->mp_valid = true;
	mode = ba.res.mode == NTFSMAC_VMODE_RW ? NTFSMAC_VMODE_RW : NTFSMAC_VMODE_RO;
	if (ba.res.reason != NTFSMAC_SREASON_NONE)
		copy_str(ro_token, sizeof ro_token, ntfsmac_report_session_reason_token(ba.res.reason));
	pthread_mutex_lock(&g_mu);
	v->session_id = ntfsmac_session_id(v->session);
	copy_str(v->mountpoint, sizeof v->mountpoint, mp.path);
	v->wk_uid = cuid;
	v->wk_gid = cgid;
	v->wk_live = true;
	ntfsmac_uev_begin(&v->uev, v->session_id);
	pthread_mutex_unlock(&g_mu);
	(void)sm_apply(v, NTFSMAC_VEV_DECIDED, mode, NTFSMAC_VREASON_NONE, NULL);

	/* Step 8: the worker. The write end of the session channel goes to it and is closed here. */
	if (ntfsmac_errwatch_pipe_open(&rd, &wr) != NTFSMAC_DMN_OK) {
		NTFSMAC_LOGE("%s: no session channel", v->bsd);
		rd = wr = -1;
		(void)sm_apply(v, NTFSMAC_VEV_MOUNT_FAILED, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_SPAWN, NULL);
		settle_failed_spawn(v, -1, -1);
		notify_token(v, NTFSMAC_NOTIFY_REFUSED, "spawn");
		goto out;
	}
	ntfsmac_errwatch_init(&v->ew);
	memset(&wrun, 0, sizeof wrun);
	wrun.device_fd = ntfsmac_session_device_fd(v->session);
	wrun.errfd_write = wr;
	wrun.mount_point = mp.path;
	wrun.read_only = mode == NTFSMAC_VMODE_RO;
	wrun.console_uid = cuid;
	wrun.console_gid = cgid;
	wrun.mount_wait_s = MOUNT_WAIT_S;
	st = ntfsmac_spawn_worker(&wrun, &child, &mounted_ro);
	if (st == NTFSMAC_DMN_E_IO)
		log_worker_exit(v, &child);
	if (worker_retry_ok(v, st, &child, rd)) {
		/*
		 * macFUSE refused the first start (exit 21) while the module instance of a session that
		 * was killed is still going away. One more start, after a short wait, with the same
		 * session, device, mount point and decision: nothing of the first start is left (no
		 * process, no mount, no S record, the device free), so it cannot double-mount. A second
		 * refusal, or any other result, is settled exactly as a failure of the first.
		 */
		NTFSMAC_LOGW("%s: worker start refused by macFUSE (exit %d); retrying once", v->bsd,
			     NTFSMAC_WORKER_EXIT_FUSE_REFUSED);
		close(wr);
		close(rd);
		sleep_ms(WORKER_RETRY_DELAY_MS);
		if (g_stopping || (v->mp_valid && mount_present(v->mp.path))) {
			NTFSMAC_LOGW("%s: the retry is dropped (daemon stopping or a mount appeared)", v->bsd);
			(void)sm_apply(v, NTFSMAC_VEV_MOUNT_FAILED, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_SPAWN, NULL);
			v->worker = child;
			settle_failed_spawn(v, -1, -1);
			notify_token(v, NTFSMAC_NOTIFY_REFUSED, "spawn");
			goto out;
		}
		if (ntfsmac_errwatch_pipe_open(&rd, &wr) != NTFSMAC_DMN_OK) {
			NTFSMAC_LOGE("%s: no session channel", v->bsd);
			rd = wr = -1;
			(void)sm_apply(v, NTFSMAC_VEV_MOUNT_FAILED, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_SPAWN, NULL);
			settle_failed_spawn(v, -1, -1);
			notify_token(v, NTFSMAC_NOTIFY_REFUSED, "spawn");
			goto out;
		}
		ntfsmac_errwatch_init(&v->ew);
		wrun.errfd_write = wr;
		st = ntfsmac_spawn_worker(&wrun, &child, &mounted_ro);
		if (st == NTFSMAC_DMN_OK)
			NTFSMAC_LOGI("%s: the worker mounted on the retry", v->bsd);
		else {
			NTFSMAC_LOGW("%s: the retry did not mount either (%s)", v->bsd, ntfsmac_dmn_status_name(st));
			if (st == NTFSMAC_DMN_E_IO)
				log_worker_exit(v, &child);
		}
	}
	if (st != NTFSMAC_DMN_OK) {
		why = st == NTFSMAC_DMN_E_DENIED    ? NTFSMAC_VREASON_ARGV_REJECTED
		      : st == NTFSMAC_DMN_E_TIMEOUT ? NTFSMAC_VREASON_MOUNT_TIMEOUT
						    : NTFSMAC_VREASON_SPAWN;
		NTFSMAC_LOGE("%s: the worker did not mount (%s)", v->bsd, ntfsmac_dmn_status_name(st));
		(void)sm_apply(v, NTFSMAC_VEV_MOUNT_FAILED, NTFSMAC_VMODE_NONE, why, NULL);
		v->worker = child;
		settle_failed_spawn(v, rd, wr);
		notify_token(v, NTFSMAC_NOTIFY_REFUSED, ntfsmac_report_vreason_token(why));
		goto out;
	}
	close(wr);
	wr = -1;
	v->worker = child;
	v->errrd = rd;
	v->started = time(NULL);
	v->write_error_noted = false;

	/* The mode the mount really has: a ro decision with a writable mount is a failure. */
	ro_final = mounted_ro; /* what the mount really is, from its MNT_RDONLY flag */
	{
		ntfsmac_volume trial;
		ntfsmac_vol_event_arg ma;

		pthread_mutex_lock(&g_mu);
		trial = v->sm;
		pthread_mutex_unlock(&g_mu);
		ma.mode = ro_final ? NTFSMAC_VMODE_RO : NTFSMAC_VMODE_RW;
		ma.reason = NTFSMAC_VREASON_NONE;
		vr = ntfsmac_volume_apply(&trial, NTFSMAC_VEV_MOUNTED, &ma);
	}
	if (vr == NTFSMAC_VRES_MODE_MISMATCH) {
		NTFSMAC_LOGE("%s: a read-only decision gave a writable mount: stopping it", v->bsd);
		(void)sm_apply(v, NTFSMAC_VEV_MOUNT_FAILED, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_MODE_MISMATCH, NULL);
		settle_failed_spawn(v, v->errrd, -1); /* kills the worker, removes the mount, settles */
		v->errrd = -1;
		notify_token(v, NTFSMAC_NOTIFY_REFUSED, "mode_mismatch");
		goto out;
	}
	if (!ro_final) {
		/* rw as decided */
	} else if (mode == NTFSMAC_VMODE_RW) {
		/* An rw decision, a read-only mount: recorded as ro (section 4.1 step 8). */
		copy_str(ro_token, sizeof ro_token, "engine_ro");
	}
	(void)sm_apply(v, NTFSMAC_VEV_MOUNTED, ro_final ? NTFSMAC_VMODE_RO : NTFSMAC_VMODE_RW,
		       NTFSMAC_VREASON_NONE, ro_final ? ro_token : "");
	timer_start(v);

	/* Step 9: say it. */
	if (!ro_final) {
		notify(v, NTFSMAC_NOTIFY_MOUNTED_RW, NULL);
	} else {
		if (ba.res.reason == NTFSMAC_SREASON_MARKER_INTERRUPTED)
			notify(v, NTFSMAC_NOTIFY_VOLMARK_INTERRUPTED,
			       "The volume is mounted read-only. If it does not mount again, run "
			       "ntfsmac repair --sync-mirror or Windows chkdsk.");
		notify_token(v, NTFSMAC_NOTIFY_MOUNTED_RO, ro_token);
	}
	(void)mw_check(-1, NULL, NULL); /* a hijack during the mount wait is seen before the first tick */
out:
	free(pv);
	free(in);
	attach_done(rq);
}

/* ------------------------------------------------------------------ */
/* Other events on the volume's queue                                  */
/* ------------------------------------------------------------------ */

static void disk_gone_run(void *arg)
{
	vol *v = arg;
	ntfsmac_vol_state s = vol_state(v);

	if (s == NTFSMAC_VOL_GONE)
		return;
	if (sm_apply(v, NTFSMAC_VEV_DISK_GONE, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_NONE, NULL) != NTFSMAC_DMN_OK)
		return;
	s = vol_state(v);
	if (s == NTFSMAC_VOL_UNMOUNTING) {
		td_args a;

		memset(&a, 0, sizeof a);
		a.kill_first = true; /* the device is gone: the worker will not end by itself */
		vol_teardown(v, &a);
	}
	pthread_mutex_lock(&g_mu);
	if (v->sm.state == NTFSMAC_VOL_GONE) {
		v->label[0] = '\0';
		v->reason_token[0] = '\0';
		v->dissent = false;
		v->apple_mounted = false;
	}
	pthread_mutex_unlock(&g_mu);
}

typedef struct unmount_req {
	vol *v;
	bool force;
	ntfsmac_dmn_status st;
	pid_t busy;
	bool waiting; /* the volume waits for Spotlight / Quick Look: answer "started" */
	dispatch_semaphore_t done;
} unmount_req;

/* The real operations of the indexer wait (ADR-028). */
typedef struct um_job {
	vol *v;
	unsigned gen;
} um_job;

static void um_retry(void *arg);

static size_t um_holders(void *ctx, pid_t *pids, size_t cap)
{
	vol *v = ctx;

	return v->mp_valid ? ntfsmac_unmount_holders(v->mp.path, pids, cap) : 0;
}

static bool um_is_indexer(void *ctx, pid_t pid)
{
	(void)ctx;
	return ntfsmac_unmount_pid_is_indexer(pid);
}

static uint64_t um_now(void *ctx)
{
	(void)ctx;
	return ext_now_ms();
}

static bool um_stopping(void *ctx)
{
	(void)ctx;
	return g_stopping != 0;
}

static void um_schedule(void *ctx, unsigned gen)
{
	um_job *j = malloc(sizeof *j);

	if (j == NULL) {
		((vol *)ctx)->um.waiting = false; /* no retry can be queued: the wait is over */
		return;
	}
	j->v = ctx;
	j->gen = gen;
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)INDEXER_STEP_MS * NSEC_PER_MSEC), j->v->q, j,
			 um_retry);
}

static const ntfsmac_umwait_ops um_ops_tmpl = { um_holders, um_is_indexer, um_now, um_stopping, um_schedule, INDEXER_WAIT_MS, NULL };

static ntfsmac_umwait_ops um_ops(vol *v)
{
	ntfsmac_umwait_ops o = um_ops_tmpl;

	o.ctx = v;
	return o;
}

/* The end of an indexer wait that did not unmount: the volume goes back to Mounted. Queue of the volume. */
static void um_wait_end(vol *v, ntfsmac_umw_after how, ntfsmac_dmn_status st, pid_t report)
{
	if (how == NTFSMAC_UMA_BUSY) {
		char name[64] = "unknown";

#if defined(__APPLE__)
		if (report > 0)
			(void)proc_name(report, name, sizeof name);
#endif
		NTFSMAC_LOGW("%s: unmount refused: busy, held by pid %d (%s)", v->bsd, (int)report, name);
	} else {
		NTFSMAC_LOGW("%s: unmount did not finish: %s", v->bsd, ntfsmac_dmn_status_name(st));
	}
	note_unmount_failed(v);
	(void)sm_apply(v, NTFSMAC_VEV_UNMOUNT_BUSY, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_NONE, NULL);
}

static void um_retry(void *arg)
{
	um_job *j = arg;
	vol *v = j->v;
	unsigned gen = j->gen;
	ntfsmac_umwait_ops o = um_ops(v);
	pid_t busy = 0, report = 0;
	ntfsmac_dmn_status st;
	ntfsmac_umw_after how;

	free(j);
	/* shutdown, the worker's teardown or a gone disk go on from here when the wait stops */
	if (ntfsmac_umwait_retry(&v->um, &o, gen,
				 vol_state(v) == NTFSMAC_VOL_UNMOUNTING && v->mp_valid && mount_present(v->mp.path)) !=
	    NTFSMAC_UMR_TRY)
		return;
	st = unmount_checked(v, "cli", false, 4u, PLAIN_UNMOUNT_MS, &busy);
	if (gen != v->um.gen)
		return; /* cancelled or restarted while the call ran */
	how = ntfsmac_umwait_after(&v->um, &o, st, busy, &report);
	if (how == NTFSMAC_UMA_BUSY || how == NTFSMAC_UMA_FAILED)
		um_wait_end(v, how, st, report);
}

static void unmount_run(void *arg)
{
	unmount_req *u = arg;
	vol *v = u->v;
	ntfsmac_vol_state s = vol_state(v);

	if (s == NTFSMAC_VOL_UNMOUNTING && v->um.waiting) {
		if (u->force) {
			/* Force wins over an indexer wait: the wait ends, the forced unmount runs on the volume. */
			ntfsmac_umwait_cancel(&v->um);
			u->st = unmount_checked(v, "cli", true, 0u, FORCE_UNMOUNT_MS, &u->busy);
			if (u->st != NTFSMAC_DMN_OK) {
				note_unmount_failed(v);
				(void)sm_apply(v, NTFSMAC_VEV_UNMOUNT_BUSY, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_NONE,
					       NULL);
			}
		} else {
			/* A second plain unmount while the wait runs: it is already under way. */
			u->st = NTFSMAC_DMN_OK;
			u->waiting = true;
		}
		goto out;
	}
	if (s != NTFSMAC_VOL_MOUNTED || !v->mp_valid) {
		u->st = NTFSMAC_DMN_E_STATE;
		goto out;
	}
	(void)sm_apply(v, NTFSMAC_VEV_UNMOUNT, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_NONE, NULL);
	/* 4 retries: the transient EBUSY of T-M2-04 (section 4.5 step 1); a bound on the whole call. */
	u->st = unmount_checked(v, "cli", u->force, u->force ? 0u : 4u,
				u->force ? FORCE_UNMOUNT_MS : PLAIN_UNMOUNT_MS, &u->busy);
	/* Spotlight / Quick Look are allowed on the volume (ADR-028): when every holder is one of them, the
	 * unmount is asked again by short jobs on this queue (um_retry), so the queue is free in between.
	 * A holder that is not an indexer is reported at once, as before, without a second scan. */
	if (!u->force && u->st == NTFSMAC_DMN_E_BUSY) {
		ntfsmac_umwait_ops o = um_ops(v);
		pid_t report = u->busy;

		if (ntfsmac_umwait_start(&v->um, &o, u->busy, &report)) {
			NTFSMAC_LOGI("%s: unmount held only by Spotlight or Quick Look, asking again for up to %u s",
				     v->bsd, INDEXER_WAIT_MS / 1000u);
			u->waiting = true;
			u->st = NTFSMAC_DMN_OK; /* the volume stays Unmounting; the request answers "started" */
			goto out;
		}
		if (report != 0)
			u->busy = report;
	}
	/* OK also covers "not a mount any more" with the mount really gone: the worker is ending. */
	if (u->st != NTFSMAC_DMN_OK) {
		note_unmount_failed(v);
		(void)sm_apply(v, NTFSMAC_VEV_UNMOUNT_BUSY, NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_NONE, NULL);
	}
	/* On success the worker ends and the tick runs the teardown. */
out:
	/* `u` belongs to the requester: it frees it after it has seen the semaphore, and leaves it
	 * (a few bytes) if it gave up waiting. Nothing here touches `u` after the signal. */
	{
		dispatch_semaphore_t done = u->done;

		dispatch_semaphore_signal(done);
		dispatch_release(done);
	}
}

/* ------------------------------------------------------------------ */
/* Disk Arbitration callbacks                                          */
/* ------------------------------------------------------------------ */

static void wait_da_ready(void)
{
	dispatch_semaphore_wait(g_da_ready, DISPATCH_TIME_FOREVER);
	dispatch_semaphore_signal(g_da_ready);
}

static void queue_attach(vol *v, bool automatic, bool request_ro, dispatch_semaphore_t done)
{
	attach_req *rq = calloc(1, sizeof *rq);

	if (rq == NULL)
		return;
	rq->v = v;
	rq->automatic = automatic;
	rq->request_ro = request_ro;
	rq->done = done;
	if (done != NULL)
		dispatch_retain(done);
	dispatch_async_f(v->q, rq, attach_run);
}

typedef struct appeared_arg {
	vol *v;
	bool has_volume_path;
	bool dissent;
	bool allow_auto;
	uint64_t size_bytes;
} appeared_arg;

/* On the volume's queue, so that it runs after the handling of an earlier disappearance. */
static void appeared_run(void *p)
{
	appeared_arg *ap = p;
	vol *v = ap->v;
	attach_req *rq;

	pthread_mutex_lock(&g_mu);
	if (v->sm.state != NTFSMAC_VOL_GONE) {
		pthread_mutex_unlock(&g_mu);
		free(ap);
		return; /* already known: nothing to restart */
	}
	ntfsmac_volume_init(&v->sm);
	v->label[0] = '\0';
	v->mountpoint[0] = '\0';
	v->reason_token[0] = '\0';
	v->session_id = 0;
	v->apple_mounted = ap->has_volume_path;
	v->dissent = ap->dissent;
	pthread_mutex_unlock(&g_mu);
	NTFSMAC_LOGI("%s: appeared (%llu bytes%s%s)", v->bsd, (unsigned long long)ap->size_bytes,
		     ap->has_volume_path ? ", mounted by another driver" : "",
		     ap->dissent ? "" : ", not handled");
	if (ap->dissent && ap->allow_auto) {
		rq = calloc(1, sizeof *rq);
		if (rq != NULL) {
			rq->v = v;
			rq->automatic = true;
			attach_run(rq); /* already on the volume's queue */
		}
	}
	free(ap);
}

static void cb_disk_appeared(const ntfsmac_disk_info *d, void *ctx)
{
	vol *v;
	appeared_arg *ap;
	bool allow_dissent, allow_auto;

	(void)ctx;
	wait_da_ready();
	if (!d->is_ntfs_candidate)
		return;
	v = vol_get_or_create(d->bsd_name);
	if (v == NULL) {
		NTFSMAC_LOGE("%s: too many volumes; the disk is not handled", d->bsd_name);
		return;
	}
	ap = calloc(1, sizeof *ap);
	if (ap == NULL)
		return;
	pthread_mutex_lock(&g_conflict_mu);
	(void)ntfsmac_conflict_detect(&g_conflict);
	allow_dissent = ntfsmac_passive_allows_dissent(&g_conflict);
	allow_auto = ntfsmac_passive_allows_automount(&g_conflict);
	pthread_mutex_unlock(&g_conflict_mu);

	/* Step 3: the dissent must be given from inside this callback. */
	ap->v = v;
	ap->has_volume_path = d->has_volume_path;
	ap->size_bytes = d->size_bytes;
	ap->allow_auto = allow_auto;
	ap->dissent = allow_dissent && ntfsmac_da_dissent(g_da, d->bsd_name) == NTFSMAC_DMN_OK;
	dispatch_async_f(v->q, ap, appeared_run);
}

static void cb_disk_disappeared(const char *bsd, void *ctx)
{
	vol *v;

	(void)ctx;
	wait_da_ready();
	v = vol_find(bsd);
	if (v != NULL)
		dispatch_async_f(v->q, v, disk_gone_run);
}

static void cb_disk_mounted(const ntfsmac_disk_info *d, void *ctx)
{
	vol *v;

	(void)ctx;
	wait_da_ready();
	v = vol_find(d->bsd_name);
	if (v == NULL)
		return;
	pthread_mutex_lock(&g_mu);
	/* Our own mount of the volume is not Apple's. */
	if (v->sm.state != NTFSMAC_VOL_MOUNTING && v->sm.state != NTFSMAC_VOL_MOUNTED &&
	    v->sm.state != NTFSMAC_VOL_UNMOUNTING)
		v->apple_mounted = true;
	pthread_mutex_unlock(&g_mu);
}

static void cb_disk_unmounted(const char *bsd, void *ctx)
{
	vol *v;

	(void)ctx;
	wait_da_ready();
	v = vol_find(bsd);
	if (v == NULL)
		return;
	pthread_mutex_lock(&g_mu);
	v->apple_mounted = false;
	pthread_mutex_unlock(&g_mu);
}

/*
 * A macFUSE disk got a volume path. The one that has the mount point of a volume that is Mounting
 * or Mounted is that session's disk: its BSD name is bound to the session (the path is compared
 * with Unicode normalisation; nothing later depends on the path).
 */
static void cb_fuse_volume_mounted(const char *bsd, const char *mount_path, void *ctx)
{
	vol *v;

	(void)ctx;
	wait_da_ready();
	pthread_mutex_lock(&g_mu);
	for (v = g_vols; v != NULL; v = v->next)
		if (v->mountpoint[0] != '\0' &&
		    (v->sm.state == NTFSMAC_VOL_MOUNTING || v->sm.state == NTFSMAC_VOL_MOUNTED) &&
		    ntfsmac_da_same_path(v->mountpoint, mount_path))
			(void)ntfsmac_uev_bind(&v->uev, v->session_id, bsd);
	pthread_mutex_unlock(&g_mu);
}

/*
 * macFUSE's disk of a mount disappeared: the mount was taken down through the file system layer
 * (`umount`, Finder eject, our own unmount). Killing the module process does not produce this.
 * Only the disk bound to the volume's current session counts.
 */
static void cb_fuse_volume_gone(const char *bsd, void *ctx)
{
	vol *v;

	(void)ctx;
	wait_da_ready();
	pthread_mutex_lock(&g_mu);
	for (v = g_vols; v != NULL; v = v->next)
		(void)ntfsmac_uev_da_gone(&v->uev, v->session_id, bsd);
	pthread_mutex_unlock(&g_mu);
}

/* ------------------------------------------------------------------ */
/* Registry callbacks for the session and the privileged requests      */
/* ------------------------------------------------------------------ */

/* Is a volume, on any BSD name, Mounting or Mounted with this journal session id? */
static bool session_live_cb(void *ctx, uint64_t session_id)
{
	vol *v;
	bool live = false;

	(void)ctx;
	if (session_id == 0)
		return false;
	pthread_mutex_lock(&g_mu);
	for (v = g_vols; v != NULL; v = v->next)
		if (v->session_id == session_id &&
		    (v->sm.state == NTFSMAC_VOL_MOUNTING || v->sm.state == NTFSMAC_VOL_MOUNTED ||
		     v->sm.state == NTFSMAC_VOL_UNMOUNTING))
			live = true;
	pthread_mutex_unlock(&g_mu);
	return live;
}

static bool priv_volume_cb(void *ctx, const char *bsd, ntfsmac_priv_volume *out)
{
	vol *v;
	bool found = false;

	(void)ctx;
	pthread_mutex_lock(&g_mu);
	v = vol_find_locked(bsd);
	if (v != NULL && v->sm.state != NTFSMAC_VOL_GONE) {
		ntfsmac_vol_state s = v->sm.state;
		bool ours = s == NTFSMAC_VOL_MOUNTING || s == NTFSMAC_VOL_MOUNTED || s == NTFSMAC_VOL_UNMOUNTING;

		found = true;
		/* A mount by another driver counts: the repair gate wants the volume unmounted. */
		out->mounted = ours || v->apple_mounted;
		out->mounted_rw = ours && (v->sm.mode == NTFSMAC_VMODE_RW ||
					   (s == NTFSMAC_VOL_MOUNTING && v->sm.planned_mode == NTFSMAC_VMODE_RW));
	}
	pthread_mutex_unlock(&g_mu);
	return found;
}

/* ------------------------------------------------------------------ */
/* Control operations                                                  */
/* ------------------------------------------------------------------ */

typedef struct vol_view {
	char bsd[32];
	char label[NTFSMAC_LABEL_PART_SIZE];
	char mountpoint[128];
	char reason[32];
	const char *state;
	const char *mount;
} vol_view;

/* Caller holds g_mu. */
static void view_locked(const vol *v, vol_view *o)
{
	copy_str(o->bsd, sizeof o->bsd, v->bsd);
	copy_str(o->label, sizeof o->label, v->label);
	copy_str(o->mountpoint, sizeof o->mountpoint, v->mountpoint);
	copy_str(o->reason, sizeof o->reason, v->reason_token);
	o->state = ntfsmac_vol_state_name(v->sm.state);
	o->mount = "none";
	if (v->sm.state == NTFSMAC_VOL_MOUNTED || v->sm.state == NTFSMAC_VOL_UNMOUNTING)
		o->mount = v->sm.mode == NTFSMAC_VMODE_RW ? "rw" : "ro";
	if (v->sm.state == NTFSMAC_VOL_FAILED && o->reason[0] == '\0')
		copy_str(o->reason, sizeof o->reason, ntfsmac_report_vreason_token(v->sm.reason));
}

static void rv_of(const vol_view *o, ntfsmac_report_vol *r)
{
	r->bsd = o->bsd;
	r->label = o->label;
	r->state = o->state;
	r->mount = o->mount;
	r->mountpoint = o->mountpoint;
	r->reason = o->reason;
}

static ntfsmac_dmn_status op_list(void *ctx, char *reply, size_t cap)
{
	static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; /* the views are static: one at a time */
	static vol_view views[MAX_VOLS];
	static ntfsmac_report_vol rv[MAX_VOLS];
	size_t n = 0, i;
	vol *v;

	(void)ctx;
	pthread_mutex_lock(&mu);
	pthread_mutex_lock(&g_mu);
	for (v = g_vols; v != NULL && n < MAX_VOLS; v = v->next) {
		if (v->sm.state == NTFSMAC_VOL_GONE)
			continue;
		/*
		 * A disk the probe found not to be NTFS at all is none of our business (the macFUSE
		 * FSKit backend makes small disk-image devices of its own, for one): not listed, as
		 * no notification is made for it. BitLocker stays visible.
		 */
		if (v->verdict != NULL && !v->verdict->is_ntfs && !v->verdict->bitlocker &&
		    v->sm.state == NTFSMAC_VOL_FAILED)
			continue;
		view_locked(v, &views[n]);
		n++;
	}
	pthread_mutex_unlock(&g_mu);
	for (i = 0; i < n; i++)
		rv_of(&views[i], &rv[i]);
	i = ntfsmac_report_list(rv, n, reply, cap);
	pthread_mutex_unlock(&mu);
	return i > 0 ? NTFSMAC_DMN_OK : NTFSMAC_DMN_E_IO;
}

static ntfsmac_dmn_status no_volume(char *reply, size_t cap)
{
	snprintf(reply, cap, "{\"ok\":false,\"error\":\"failed\",\"reason\":\"no_volume\"}");
	return NTFSMAC_DMN_E_INVAL;
}

static ntfsmac_dmn_status op_status(void *ctx, const char *bsd, char *reply, size_t cap)
{
	static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
	static ntfsmac_probe_verdict pv;
	vol_view view;
	ntfsmac_report_vol rv;
	vol *v;
	bool have_pv = false, have_entry = false;
	uint64_t serial = 0;
	jq_peek_arg pk;
	size_t n;

	(void)ctx;
	pthread_mutex_lock(&mu); /* the verdict copy is static: one status at a time (before g_mu) */
	pthread_mutex_lock(&g_mu);
	v = vol_find_locked(bsd);
	if (v == NULL || v->sm.state == NTFSMAC_VOL_GONE) {
		pthread_mutex_unlock(&g_mu);
		pthread_mutex_unlock(&mu);
		return no_volume(reply, cap);
	}
	view_locked(v, &view);
	if (v->verdict != NULL) {
		memcpy(&pv, v->verdict, sizeof pv);
		have_pv = true;
		serial = pv.serial;
	}
	pthread_mutex_unlock(&g_mu);
	memset(&pk, 0, sizeof pk);
	if (have_pv && serial != 0) {
		pk.serial = serial;
		dispatch_sync_f(g_journal_q, &pk, jq_peek);
		have_entry = pk.present;
		/*
		 * The entry of a session that is running now is not a "left open" one: its marker and
		 * its lost-flush warning describe what a crash would leave, so the member is left out
		 * while the session is live (it shows again when the session has ended).
		 */
		if (have_entry && session_live_cb(NULL, pk.e.session_id))
			have_entry = false;
	}
	rv_of(&view, &rv);
	n = ntfsmac_report_status(&rv, have_pv ? &pv : NULL, have_entry ? &pk.e : NULL, reply, cap);
	pthread_mutex_unlock(&mu);
	return n > 0 ? NTFSMAC_DMN_OK : NTFSMAC_DMN_E_IO;
}

/* The reply of mount and unmount: the volume after the call, with a `result` word. */
static void reply_volume(const char *result, vol *v, char *reply, size_t cap)
{
	vol_view view;
	ntfsmac_report_vol rv;
	char tmp[2048];

	pthread_mutex_lock(&g_mu);
	view_locked(v, &view);
	pthread_mutex_unlock(&g_mu);
	rv_of(&view, &rv);
	if (ntfsmac_report_status(&rv, NULL, NULL, tmp, sizeof tmp) == 0 || strncmp(tmp, "{\"ok\":true,", 11) != 0) {
		snprintf(reply, cap, "{\"ok\":true,\"result\":\"%s\"}", result);
		return;
	}
	snprintf(reply, cap, "{\"ok\":true,\"result\":\"%s\",%s", result, tmp + 11);
}

static bool wait_sem(dispatch_semaphore_t s, unsigned ms)
{
	return dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC)) == 0;
}

/*
 * `mount`: the attach flow takes 20 s and more, the socket answers in 5, so the request starts the
 * flow and waits a short time. A reply with result "started" means: look at `list`.
 */
static ntfsmac_dmn_status op_mount(void *ctx, const char *bsd, bool read_only, char *reply, size_t cap)
{
	vol *v = vol_find(bsd);
	dispatch_semaphore_t done;
	ntfsmac_dmn_status st;
	bool accepts, apple;
	ntfsmac_conflict_report cr;

	(void)ctx;
	if (v == NULL)
		return no_volume(reply, cap);
	pthread_mutex_lock(&g_mu);
	accepts = ntfsmac_volume_accepts_mount(&v->sm);
	apple = v->apple_mounted;
	pthread_mutex_unlock(&g_mu);
	if (!accepts) {
		snprintf(reply, cap, "{\"ok\":false,\"error\":\"failed\",\"reason\":\"wrong_state\"}");
		return NTFSMAC_DMN_E_STATE;
	}
	pthread_mutex_lock(&g_conflict_mu);
	(void)ntfsmac_conflict_detect(&g_conflict);
	cr = g_conflict;
	pthread_mutex_unlock(&g_conflict_mu);
	st = ntfsmac_passive_check_manual_mount(&cr, apple);
	if (st != NTFSMAC_DMN_OK) {
		snprintf(reply, cap, "{\"ok\":false,\"error\":\"denied\",\"reason\":\"passive_mode\"}");
		return NTFSMAC_DMN_E_DENIED;
	}
	done = dispatch_semaphore_create(0);
	if (done == NULL)
		return NTFSMAC_DMN_E_IO;
	/* A request is not an automatic attach (section 3.2, `automatic` false). Our dissent stays. */
	(void)ntfsmac_da_dissent(g_da, bsd);
	queue_attach(v, false, read_only, done);
	st = wait_sem(done, REQUEST_WAIT_MS) ? NTFSMAC_DMN_OK : NTFSMAC_DMN_E_TIMEOUT;
	dispatch_release(done);
	{
		ntfsmac_vol_state s = vol_state(v);

		reply_volume(s == NTFSMAC_VOL_MOUNTED ? "mounted"
			     : (s == NTFSMAC_VOL_FAILED ? "failed" : "started"),
			     v, reply, cap);
	}
	(void)st;
	return NTFSMAC_DMN_OK;
}

static ntfsmac_dmn_status op_unmount(void *ctx, const char *bsd, bool force, char *reply, size_t cap)
{
	vol *v = vol_find(bsd);
	unmount_req *u;
	bool seen;
	ntfsmac_dmn_status st;

	(void)ctx;
	if (v == NULL)
		return no_volume(reply, cap);
	u = calloc(1, sizeof *u);
	if (u == NULL)
		return NTFSMAC_DMN_E_IO;
	u->v = v;
	u->force = force;
	u->done = dispatch_semaphore_create(0);
	if (u->done == NULL) {
		free(u);
		return NTFSMAC_DMN_E_IO;
	}
	dispatch_retain(u->done); /* one for unmount_run, one for this function */
	dispatch_async_f(v->q, u, unmount_run);
	/* The control socket answers within 5 s: wait less than that, the rest shows in `list`. */
	seen = wait_sem(u->done, REQUEST_WAIT_MS + 1000u);
	if (!seen) {
		/* The queue is busy; the request still runs. The result is looked up in `list`. */
		dispatch_release(u->done);
		snprintf(reply, cap, "{\"ok\":true,\"result\":\"started\"}");
		return NTFSMAC_DMN_OK; /* `u` is leaked on purpose: unmount_run may still write to it */
	}
	dispatch_release(u->done);
	st = u->st;
	if (st == NTFSMAC_DMN_OK) {
		snprintf(reply, cap, u->waiting ? "{\"ok\":true,\"result\":\"started\"}"
						 : "{\"ok\":true,\"result\":\"unmounting\"}");
	} else if (st == NTFSMAC_DMN_E_BUSY) {
		snprintf(reply, cap, "{\"ok\":false,\"error\":\"failed\",\"status\":\"busy\",\"busy_pid\":%d}",
			 (int)u->busy);
	} else if (st == NTFSMAC_DMN_E_STATE) {
		snprintf(reply, cap, "{\"ok\":false,\"error\":\"failed\",\"reason\":\"wrong_state\"}");
	} else {
		reply[0] = '\0';
	}
	free(u);
	return st;
}

typedef struct doctor_acc {
	ntfsmac_doctor_finding f[64];
	size_t n;
} doctor_acc;

static int doctor_cb(const ntfsmac_doctor_finding *f, void *ctx)
{
	doctor_acc *a = ctx;

	if (a->n < sizeof a->f / sizeof a->f[0])
		a->f[a->n++] = *f;
	return 0;
}

#define EXT_QUERY_WAIT_MS 3000u /* under the 5 s of the control socket */

static uint64_t ext_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/*
 * The macFUSE extension state (doctor.c asks for a hook; ARCHITECTURE.md section 8.4). The election
 * of an FSKit module is per user, so the query runs as the user at the console, in their session:
 * `launchctl asuser UID sudo -n -u #UID pluginkit -m -v -i <id>`, as notify.c starts osascript.
 * Fixed paths and arguments, a fixed environment, no shell; the output is read through a pipe up
 * to NTFSMAC_EXT_OUT_MAX bytes and the child group is killed after EXT_QUERY_WAIT_MS. Nobody at
 * the console, a failed start, a timeout, a non-zero exit or too much output: unknown.
 */
static ntfsmac_doctor_ext_state ext_state_cb(void *ctx)
{
	uid_t uid;
	gid_t gid;
	char uidtxt[16], who[24];
	char out[NTFSMAC_EXT_OUT_MAX + 1];
	size_t got = 0;
	int p[2], st = 0;
	bool ok = true, reaped = false;
	pid_t pid = -1;
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t at;
	char *envp[] = {(char *)"PATH=/usr/bin:/bin:/usr/sbin:/sbin", NULL};
	char *argv[] = {(char *)"/bin/launchctl", (char *)"asuser", uidtxt, (char *)"/usr/bin/sudo",
			(char *)"-n", (char *)"-u", who, (char *)"/usr/bin/pluginkit",
			(char *)"-m", (char *)"-v", (char *)"-i",
			(char *)NTFSMAC_EXT_MODULE_ID, NULL};
	uint64_t deadline;

	(void)ctx;
	if (!console_ids(&uid, &gid))
		return NTFSMAC_EXT_UNKNOWN;
	snprintf(uidtxt, sizeof uidtxt, "%u", (unsigned)uid);
	snprintf(who, sizeof who, "#%u", (unsigned)uid);
	if (pipe(p) != 0)
		return NTFSMAC_EXT_UNKNOWN;
	(void)fcntl(p[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(p[1], F_SETFD, FD_CLOEXEC);
	(void)fcntl(p[0], F_SETFL, O_NONBLOCK);
	if (posix_spawn_file_actions_init(&fa) != 0) {
		close(p[0]);
		close(p[1]);
		return NTFSMAC_EXT_UNKNOWN;
	}
	if (posix_spawnattr_init(&at) != 0) {
		posix_spawn_file_actions_destroy(&fa);
		close(p[0]);
		close(p[1]);
		return NTFSMAC_EXT_UNKNOWN;
	}
	(void)posix_spawnattr_setpgroup(&at, 0);
	(void)posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP);
	(void)posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	(void)posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	(void)posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	if (posix_spawn(&pid, argv[0], &fa, &at, argv, envp) != 0)
		ok = false;
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&at);
	close(p[1]);
	if (ok) {
		deadline = ext_now_ms() + EXT_QUERY_WAIT_MS;
		for (;;) {
			struct pollfd pf = {p[0], POLLIN, 0};
			uint64_t now = ext_now_ms();
			ssize_t r;

			if (now >= deadline) {
				ok = false;
				break;
			}
			r = poll(&pf, 1, (int)(deadline - now));
			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0) {
				ok = false;
				break;
			}
			r = read(p[0], out + got, sizeof out - got);
			if (r < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			if (r < 0) {
				ok = false;
				break;
			}
			if (r == 0)
				break; /* end of output */
			got += (size_t)r;
			if (got > NTFSMAC_EXT_OUT_MAX) {
				ok = false;
				break;
			}
		}
	}
	close(p[0]);
	if (pid > 0) {
		if (ok) {
			/* the pipe is closed by the child's exit; the status follows at once */
			uint64_t until = ext_now_ms() + 1000u;

			for (;;) {
				pid_t w = waitpid(pid, &st, WNOHANG);

				if (w == pid) {
					reaped = true;
					break;
				}
				if (w < 0 && errno != EINTR)
					break;
				if (ext_now_ms() >= until)
					break;
				{
					struct timespec ts = {0, 10 * 1000000L};

					nanosleep(&ts, NULL);
				}
			}
			if (!reaped || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
				ok = false;
		}
		if (!reaped) {
			(void)killpg(pid, SIGKILL);
			(void)kill(pid, SIGKILL);
			(void)waitpid(pid, &st, 0);
		}
	}
	return ok ? ntfsmac_doctor_ext_parse(out, got) : NTFSMAC_EXT_UNKNOWN;
}

static ntfsmac_dmn_status op_doctor(void *ctx, char *reply, size_t cap)
{
	doctor_acc *a = calloc(1, sizeof *a);
	ntfsmac_doctor_env env;
	ntfsmac_dmn_status st;
	size_t n;

	(void)ctx;
	if (a == NULL)
		return NTFSMAC_DMN_E_IO;
	memset(&env, 0, sizeof env);
	env.journal_dir = JOURNAL_DIR;
	env.ext_fn = ext_state_cb;
	st = ntfsmac_doctor_run_env(&env, doctor_cb, a);
	n = st == NTFSMAC_DMN_OK ? ntfsmac_report_doctor(a->f, a->n, reply, cap) : 0;
	free(a);
	return n > 0 ? NTFSMAC_DMN_OK : (st != NTFSMAC_DMN_OK ? st : NTFSMAC_DMN_E_IO);
}

/* ---- the privileged requests: on the journal queue ---- */

typedef struct priv_call {
	const ntfsmac_ctl_peer *peer;
	const char *bsd, *action, *label, *serial;
	char *reply;
	size_t cap;
	bool is_repair;
	ntfsmac_dmn_status st;
} priv_call;

static void jq_priv(void *p)
{
	priv_call *c = p;

	if (c->is_repair)
		c->st = ntfsmac_priv_repair(&g_priv, c->peer, c->bsd, c->action, c->label, c->serial,
					    c->reply, c->cap);
	else
		c->st = ntfsmac_priv_journal_clear(&g_priv, c->peer, c->bsd, c->label, c->serial,
						   c->reply, c->cap);
}

static ntfsmac_dmn_status op_journal_clear(void *ctx, const ntfsmac_ctl_peer *peer, const char *bsd,
					   const char *label, const char *serial, char *reply, size_t cap)
{
	priv_call c;

	(void)ctx;
	memset(&c, 0, sizeof c);
	c.peer = peer;
	c.bsd = bsd;
	c.label = label;
	c.serial = serial;
	c.reply = reply;
	c.cap = cap;
	dispatch_sync_f(g_journal_q, &c, jq_priv);
	return c.st;
}

static ntfsmac_dmn_status op_repair(void *ctx, const ntfsmac_ctl_peer *peer, const char *bsd,
				    const char *action, const char *label, const char *serial,
				    char *reply, size_t cap)
{
	priv_call c;

	(void)ctx;
	memset(&c, 0, sizeof c);
	c.peer = peer;
	c.bsd = bsd;
	c.action = action;
	c.label = label;
	c.serial = serial;
	c.reply = reply;
	c.cap = cap;
	c.is_repair = true;
	dispatch_sync_f(g_journal_q, &c, jq_priv);
	return c.st;
}

/* Parent process id of `pid`, or -1. */
static pid_t parent_of(pid_t pid)
{
	struct proc_bsdinfo bi;

	if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi)
		return -1;
	return (pid_t)bi.pbi_ppid;
}

/*
 * Check 2 of ADR-019: is `pid` a live probe, volmark or worker of this daemon? Every child the
 * daemon starts is in its own process group and has the daemon as its parent, so a process whose
 * parent is this daemon is one of them (or a helper of the daemon: no client is ever one). The
 * workers are also known by pid. The checker walks the parent chain and the process groups of
 * the peer through this function.
 */
static bool op_is_spawned(void *ctx, pid_t pid)
{
	vol *v;
	bool hit = false;

	(void)ctx;
	if (pid <= 1)
		return false;
	if (parent_of(pid) == getpid())
		return true;
	pthread_mutex_lock(&g_mu);
	for (v = g_vols; v != NULL; v = v->next)
		if (v->wk_live && v->worker.pid == pid)
			hit = true;
	pthread_mutex_unlock(&g_mu);
	return hit;
}

static const ntfsmac_ctl_ops g_ops = {
	.list = op_list,
	.status = op_status,
	.mount = op_mount,
	.unmount = op_unmount,
	.doctor = op_doctor,
	.journal_clear = op_journal_clear,
	.repair = op_repair,
	.is_spawned = op_is_spawned,
};

/* ------------------------------------------------------------------ */
/* Start and stop                                                      */
/* ------------------------------------------------------------------ */

static int reconcile_cb(const ntfsmac_journal_entry *e, ntfsmac_reconcile_action action,
			const ntfsmac_live_mount *m, void *ctx)
{
	ntfsmac_dmn_status st;

	(void)ctx;
	(void)e;
	if (action == NTFSMAC_RECONCILE_KEEP_ENTRY) {
		NTFSMAC_LOGI("restart: a journal entry without a live mount stays (the next mount is read-only)");
		return 0;
	}
	/* No re-adoption: a worker of the earlier daemon is not looked up, so its mount is removed. */
	st = ntfsmac_mw_force_unmount(m->mount_point, FORCE_UNMOUNT_MS);
	NTFSMAC_LOGW("restart: forced unmount of a mount of the earlier daemon: %s", ntfsmac_dmn_status_name(st));
	return 0;
}

static void shutdown_vol(void *arg)
{
	vol *v = arg;
	ntfsmac_vol_state s = vol_state(v);
	td_args a;
	unsigned waited = 0;

	if (s != NTFSMAC_VOL_MOUNTED && s != NTFSMAC_VOL_UNMOUNTING)
		return;
	/* A plain unmount first, so that the worker ends the way section 4.5 describes. An indexer wait
	 * (ADR-028) ends here: the plain unmount runs as for Mounted, so a session that can end cleanly does. */
	if (s == NTFSMAC_VOL_UNMOUNTING && v->um.waiting)
		ntfsmac_umwait_cancel(&v->um);
	else if (s == NTFSMAC_VOL_UNMOUNTING)
		goto settle;
	if (v->mp_valid && (s == NTFSMAC_VOL_MOUNTED || s == NTFSMAC_VOL_UNMOUNTING)) {
		pid_t busy;

		(void)unmount_checked(v, "shutdown", false, 3u, PLAIN_UNMOUNT_MS, &busy);
	}
settle:
	memset(&a, 0, sizeof a);
	while (v->worker.pid > 1 && waited < 8000u) {
		bool exited = false;
		int es = -1;

		if (ntfsmac_child_poll(&v->worker, &exited, &es) != NTFSMAC_DMN_OK || exited) {
			a.worker_reaped = true;
			a.exit_status = es;
			break;
		}
		sleep_ms(50);
		waited += 50;
	}
	if (!a.worker_reaped)
		a.kill_first = true;
	vol_teardown(v, &a);
}

/*
 * The volumes are settled in parallel (each on its own queue) and the wait is bounded, so that
 * several volumes cannot add up past the plist's ExitTimeOut of 60 s: a volume that is not
 * settled in SHUTDOWN_WAIT_MS is left to its journal entry (the next start finds it, and a rw
 * session without a clean end is never taken as clean). One volume needs at most 8 s for the
 * worker, 6 s for the plain unmount and a few seconds for the settlement.
 */
static void stop_all(void)
{
	vol *v;
	dispatch_group_t grp = dispatch_group_create();

	g_stopping = 1;
	pthread_mutex_lock(&g_mu);
	for (v = g_vols; v != NULL; v = v->next)
		dispatch_group_async_f(grp, v->q, v, shutdown_vol);
	pthread_mutex_unlock(&g_mu);
	if (dispatch_group_wait(grp, dispatch_time(DISPATCH_TIME_NOW, (int64_t)SHUTDOWN_WAIT_MS * NSEC_PER_MSEC)) != 0)
		NTFSMAC_LOGE("ntfsmacd: not every volume was settled in %u s; stopping anyway", SHUTDOWN_WAIT_MS / 1000u);
}

static void on_term(void *arg)
{
	(void)arg;
	NTFSMAC_LOGI("ntfsmacd: stopping");
	if (g_xpc != NULL)
		ntfsmac_xpc_stop(g_xpc);
	if (g_ctl != NULL)
		ntfsmac_ctl_stop(g_ctl);
	g_ctl = NULL;
	if (g_da != NULL)
		ntfsmac_da_stop(g_da);
	g_da = NULL;
	stop_all();
	if (g_journal != NULL)
		ntfsmac_journal_close(g_journal);
	exit(0);
}

static void usage(void)
{
	fputs("usage: ntfsmacd [--automount-mode=ro|auto]"
#if defined(NTFSMAC_TEST_SEAMS)
	      " [--socket PATH] [--journal-dir DIR] [--child-log FILE] [--no-marker] [--console-uid UID]"
#endif
	      "\n",
	      stderr);
}

static bool parse_args(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "--automount-mode=ro") == 0)
			g_opt.automount_ro = true;
		else if (strcmp(a, "--automount-mode=auto") == 0)
			g_opt.automount_ro = false;
#if defined(NTFSMAC_TEST_SEAMS)
		else if (strcmp(a, "--socket") == 0 && i + 1 < argc)
			g_opt.socket_path = argv[++i];
		else if (strcmp(a, "--journal-dir") == 0 && i + 1 < argc)
			g_opt.journal_dir = argv[++i];
		else if (strcmp(a, "--child-log") == 0 && i + 1 < argc)
			g_opt.child_log = argv[++i];
		else if (strcmp(a, "--no-marker") == 0)
			g_opt.marker_disabled = true;
		else if (strcmp(a, "--console-uid") == 0 && i + 1 < argc) {
			g_opt.console_uid = (uid_t)strtoul(argv[++i], NULL, 10);
			g_opt.has_console_uid = true;
		}
#endif
		else
			return false;
	}
	return true;
}

int main(int argc, char **argv)
{
	ntfsmac_spawn_config cfg;
	ntfsmac_da_callbacks cb;
	ntfsmac_ctl_env xenv;
	ntfsmac_dmn_status st;
	ntfsmac_journal_status js;
	dispatch_source_t sig_term, sig_int;

	/* The worker's start goes through here once, inside the console user's session (spawn.c). */
	if (argc >= 2 && strcmp(argv[1], NTFSMAC_EXEC_AS_FLAG) == 0)
		return ntfsmac_spawn_exec_as(argc, argv);
	if (!parse_args(argc, argv)) {
		usage();
		return 2;
	}
	if (geteuid() != 0) {
		fputs("ntfsmacd: must run as root\n", stderr);
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	umask(077);
	/* Before any thread: the daemon's own disk I/O and every child it starts are passive, so a disk
	 * image's diskimages-helper is not throttled by them (docs/spikes/M5-perf.md). */
	if (ntfsmac_spawn_io_passive() != 0)
		fprintf(stderr, "ntfsmacd: passive disk I/O not set: %s\n", strerror(errno));

	/* The spawn module: manifest paths, the sandbox on, the user `nobody` for probe and volmark. */
	ntfsmac_spawn_config_default(&cfg);
#if defined(NTFSMAC_TEST_SEAMS)
	if (g_opt.child_log != NULL) {
		int fd = open(g_opt.child_log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

		if (fd >= 0)
			cfg.child_stderr_fd = fd;
	}
#endif
	ntfsmac_spawn_config_set(&cfg);

	/* The journal; its parent directory is ours to create (root, 0755). */
	if (strcmp(JOURNAL_DIR, NTFSMAC_JOURNAL_DEFAULT_DIR) == 0 && mkdir(JOURNAL_PARENT, 0755) != 0 &&
	    errno != EEXIST) {
		fprintf(stderr, "ntfsmacd: cannot create %s: %s\n", JOURNAL_PARENT, strerror(errno));
		return 1;
	}
	js = ntfsmac_journal_open(JOURNAL_DIR, &g_journal);
	if (js != NTFSMAC_JOURNAL_OK) {
		fprintf(stderr, "ntfsmacd: cannot open the journal %s (status %d)\n", JOURNAL_DIR, (int)js);
		return 1;
	}

	memset(&g_scfg, 0, sizeof g_scfg);
	g_scfg.automount_ro = g_opt.automount_ro;
	g_scfg.evidence_rule_enabled = false; /* section 3.5 rule 2: off until U12 is resolved */
#if defined(NTFSMAC_TEST_SEAMS)
	g_scfg.marker_disabled = g_opt.marker_disabled;
#endif

	g_journal_q = dispatch_queue_create("org.ntfs-macos.journal", DISPATCH_QUEUE_SERIAL);
	g_da_ready = dispatch_semaphore_create(0);
	if (g_journal_q == NULL || g_da_ready == NULL)
		return 1;

	/* Restart reconciliation, before anything new can attach (section 7). */
	st = ntfsmac_reconcile_run(g_journal, reconcile_cb, NULL);
	if (st != NTFSMAC_DMN_OK)
		NTFSMAC_LOGW("restart reconciliation: %s", ntfsmac_dmn_status_name(st));

	/* The session facts. `da` is filled in below, before the first callback is let through. */
	ntfsmac_session_sys_real_init(&g_real, NULL);
	ntfsmac_priv_sys_real_init(&g_priv_real, &g_real.sys, priv_volume_cb, session_live_cb, NULL);
	g_priv.sys = &g_priv_real.sys;
	g_priv.journal = g_journal;
	(void)ntfsmac_conflict_detect(&g_conflict);
	if (g_conflict.passive)
		NTFSMAC_LOGW("another NTFS driver is installed: passive mode");

	st = ntfsmac_ctl_start(SOCKET_PATH, &g_ops, NULL, &g_ctl);
	if (st != NTFSMAC_DMN_OK) {
		fprintf(stderr, "ntfsmacd: cannot start the control socket %s: %s\n", SOCKET_PATH,
			ntfsmac_dmn_status_name(st));
		return 1;
	}

	/* The privileged door. env.authz comes from the build's default only (ADR-019, check 4). */
	xenv = ntfsmac_ctl_default_env_xpc();
	xenv.authz = ntfsmac_ctl_default_authz();
	NTFSMAC_LOGI("check 4 of ADR-019: %s", ntfsmac_ctl_authz_name(xenv.authz));
	st = ntfsmac_xpc_start(NTFSMAC_CTL_XPC_SERVICE, &g_ops, NULL, &xenv, &g_xpc);
	if (st != NTFSMAC_DMN_OK)
		NTFSMAC_LOGE("the privileged XPC service %s did not start (%s): journal-clear and repair "
			     "are unavailable (is %s installed?)",
			     NTFSMAC_CTL_XPC_SERVICE, ntfsmac_dmn_status_name(st), NTFSMAC_CTL_REQUIREMENT_FILE);

	mw_start();

	/* Disk Arbitration last: from here on disks are handled. */
	memset(&cb, 0, sizeof cb);
	cb.disk_appeared = cb_disk_appeared;
	cb.disk_disappeared = cb_disk_disappeared;
	cb.disk_mounted = cb_disk_mounted;
	cb.disk_unmounted = cb_disk_unmounted;
	cb.fuse_volume_gone = cb_fuse_volume_gone;
	cb.fuse_volume_mounted = cb_fuse_volume_mounted;
	st = ntfsmac_da_start(&cb, NULL, &g_da);
	if (st != NTFSMAC_DMN_OK) {
		fprintf(stderr, "ntfsmacd: Disk Arbitration did not start: %s\n", ntfsmac_dmn_status_name(st));
		return 1;
	}
	g_real.da = g_da; /* non-NULL: a NULL here would make every binding fail closed */
	dispatch_semaphore_signal(g_da_ready);

	signal(SIGTERM, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	sig_term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
	sig_int = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
	dispatch_source_set_event_handler_f(sig_term, on_term);
	dispatch_source_set_event_handler_f(sig_int, on_term);
	dispatch_resume(sig_term);
	dispatch_resume(sig_int);

	NTFSMAC_LOGI("ntfsmacd: started");
	dispatch_main();
	return 0;
}
