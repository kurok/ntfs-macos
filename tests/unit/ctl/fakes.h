/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_CTL_TEST_FAKES_H
#define NTFSMAC_CTL_TEST_FAKES_H

/*
 * Fake operations and fake system facts for the control socket tests.
 * Nothing here calls the operating system, so every peer check can be made
 * to pass or fail on its own.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ctl_priv.h"

/* ---------------- operations ---------------- */

/*
 * The fake state is written by the test thread and read by the server thread
 * (and the other way round for the call counters). The socket I/O between the
 * two orders these accesses in real time, but ThreadSanitizer does not see an
 * ordering through a socket. fake_sync() gives it one it can see: a lock and
 * unlock of one mutex. The fakes call it on entry and on exit; the tests call
 * it (through dial() and slurp()) after writing fake state and after reading
 * a reply. The fake state is never touched by two threads at the same time.
 */
static pthread_mutex_t fake_sync_lock = PTHREAD_MUTEX_INITIALIZER;

static inline void fake_sync(void)
{
	pthread_mutex_lock(&fake_sync_lock);
	pthread_mutex_unlock(&fake_sync_lock);
}


typedef struct fake_ops_state {
	int calls[NTFSMAC_CTL_CMD_COUNT_];
	char bsd[64];
	char label[300];
	char serial[16];
	char action[16];
	bool read_only, force;
	ntfsmac_ctl_peer peer;
	/* what the next operation does */
	ntfsmac_dmn_status status;
	const char *reply; /* NULL: leave the reply empty */
	unsigned delay_ms; /* each operation blocks this long first (a slow operation) */
	/* spawned pids for is_spawned */
	pid_t spawned[8];
	int nspawned;
} fake_ops_state;

static inline int fake_total_calls(const fake_ops_state *s)
{
	int i, n = 0;

	for (i = 0; i < NTFSMAC_CTL_CMD_COUNT_; i++)
		n += s->calls[i];
	return n;
}

static inline ntfsmac_dmn_status fake_finish(fake_ops_state *s, ntfsmac_ctl_cmd c, char *reply,
					     size_t cap)
{
	ntfsmac_dmn_status st;

	fake_sync();
	if (s->delay_ms != 0) {
		struct timespec ts;

		ts.tv_sec = (time_t)(s->delay_ms / 1000u);
		ts.tv_nsec = (long)(s->delay_ms % 1000u) * 1000000L;
		while (nanosleep(&ts, &ts) != 0)
			;
	}
	s->calls[c]++;
	if (s->reply != NULL)
		snprintf(reply, cap, "%s", s->reply);
	st = s->status;
	fake_sync();
	return st;
}

static inline ntfsmac_dmn_status f_list(void *ctx, char *reply, size_t cap)
{
	return fake_finish(ctx, NTFSMAC_CTL_CMD_LIST, reply, cap);
}

static inline ntfsmac_dmn_status f_status(void *ctx, const char *bsd, char *reply, size_t cap)
{
	fake_ops_state *s = ctx;

	snprintf(s->bsd, sizeof s->bsd, "%s", bsd);
	return fake_finish(s, NTFSMAC_CTL_CMD_STATUS, reply, cap);
}

static inline ntfsmac_dmn_status f_mount(void *ctx, const char *bsd, bool ro, char *reply,
					 size_t cap)
{
	fake_ops_state *s = ctx;

	snprintf(s->bsd, sizeof s->bsd, "%s", bsd);
	s->read_only = ro;
	return fake_finish(s, NTFSMAC_CTL_CMD_MOUNT, reply, cap);
}

static inline ntfsmac_dmn_status f_unmount(void *ctx, const char *bsd, bool force, char *reply,
					   size_t cap)
{
	fake_ops_state *s = ctx;

	snprintf(s->bsd, sizeof s->bsd, "%s", bsd);
	s->force = force;
	return fake_finish(s, NTFSMAC_CTL_CMD_UNMOUNT, reply, cap);
}

static inline ntfsmac_dmn_status f_doctor(void *ctx, char *reply, size_t cap)
{
	return fake_finish(ctx, NTFSMAC_CTL_CMD_DOCTOR, reply, cap);
}

static inline ntfsmac_dmn_status f_journal_clear(void *ctx, const ntfsmac_ctl_peer *peer,
						 const char *bsd, const char *label,
						 const char *serial, char *reply, size_t cap)
{
	fake_ops_state *s = ctx;

	s->peer = *peer;
	snprintf(s->bsd, sizeof s->bsd, "%s", bsd);
	snprintf(s->label, sizeof s->label, "%s", label);
	snprintf(s->serial, sizeof s->serial, "%s", serial);
	return fake_finish(s, NTFSMAC_CTL_CMD_JOURNAL_CLEAR, reply, cap);
}

static inline ntfsmac_dmn_status f_repair(void *ctx, const ntfsmac_ctl_peer *peer, const char *bsd,
					  const char *action, const char *label,
					  const char *serial, char *reply, size_t cap)
{
	fake_ops_state *s = ctx;

	s->peer = *peer;
	snprintf(s->bsd, sizeof s->bsd, "%s", bsd);
	snprintf(s->action, sizeof s->action, "%s", action);
	snprintf(s->label, sizeof s->label, "%s", label);
	snprintf(s->serial, sizeof s->serial, "%s", serial);
	return fake_finish(s, NTFSMAC_CTL_CMD_REPAIR, reply, cap);
}

static inline bool f_is_spawned(void *ctx, pid_t pid)
{
	const fake_ops_state *s = ctx;
	int i;

	for (i = 0; i < s->nspawned; i++)
		if (s->spawned[i] == pid)
			return true;
	return false;
}

static inline ntfsmac_ctl_ops fake_ops(void)
{
	ntfsmac_ctl_ops o;

	memset(&o, 0, sizeof o);
	o.list = f_list;
	o.status = f_status;
	o.mount = f_mount;
	o.unmount = f_unmount;
	o.doctor = f_doctor;
	o.journal_clear = f_journal_clear;
	o.repair = f_repair;
	o.is_spawned = f_is_spawned;
	return o;
}

/* ---------------- system facts ---------------- */

typedef struct fake_proc {
	pid_t pid, ppid, pgid;
	ntfsmac_ctl_sys_proc result; /* what the lookup of this pid answers */
} fake_proc;

typedef struct fake_sys_state {
	fake_proc procs[16];
	int nprocs;
	ntfsmac_ctl_sys_answer signed_answer;
	ntfsmac_ctl_sys_answer authz_answer;
	ntfsmac_ctl_sys_answer right_answer; /* the stored definition of the right */
	int signed_calls, authz_calls, proc_calls, right_calls;
	uint8_t last_form[NTFSMAC_CTL_AUTH_FORM_BYTES];
	ntfsmac_ctl_peer socket_peer; /* what peer_from_fd returns */
	int peer_rc;
} fake_sys_state;

static inline ntfsmac_ctl_sys_proc fs_proc_parent(void *ctx, pid_t pid, pid_t *ppid, pid_t *pgid)
{
	fake_sys_state *s = ctx;
	int i;

	ntfsmac_ctl_sys_proc r = NTFSMAC_CTL_PROC_GONE;

	fake_sync();
	s->proc_calls++;
	for (i = 0; i < s->nprocs; i++) {
		if (s->procs[i].pid == pid) {
			if (s->procs[i].result == NTFSMAC_CTL_PROC_OK) {
				*ppid = s->procs[i].ppid;
				*pgid = s->procs[i].pgid;
			}
			r = s->procs[i].result;
			break;
		}
	}
	fake_sync();
	return r;
}

static inline ntfsmac_ctl_sys_answer fs_signed(void *ctx, const ntfsmac_ctl_peer *peer)
{
	fake_sys_state *s = ctx;
	ntfsmac_ctl_sys_answer a;

	(void)peer;
	fake_sync();
	s->signed_calls++;
	a = s->signed_answer;
	fake_sync();
	return a;
}

static inline ntfsmac_ctl_sys_answer fs_authz(void *ctx, const uint8_t form[NTFSMAC_CTL_AUTH_FORM_BYTES])
{
	fake_sys_state *s = ctx;
	ntfsmac_ctl_sys_answer a;

	fake_sync();
	s->authz_calls++;
	memcpy(s->last_form, form, sizeof s->last_form);
	a = s->authz_answer;
	fake_sync();
	return a;
}

static inline ntfsmac_ctl_sys_answer fs_right(void *ctx)
{
	fake_sys_state *s = ctx;
	ntfsmac_ctl_sys_answer a;

	fake_sync();
	s->right_calls++;
	a = s->right_answer;
	fake_sync();
	return a;
}

static inline int fs_peer_from_fd(void *ctx, int fd, ntfsmac_ctl_peer *out)
{
	fake_sys_state *s = ctx;
	int rc;

	(void)fd;
	fake_sync();
	*out = s->socket_peer;
	rc = s->peer_rc;
	fake_sync();
	return rc;
}

/*
 * A system where every check passes for pid 500: parent chain 500 -> 400 ->
 * 1, none spawned, signed, authorization granted.
 */
static inline void fake_sys_init(fake_sys_state *s, ntfsmac_ctl_sys *sys)
{
	memset(s, 0, sizeof *s);
	s->procs[0] = (fake_proc){500, 400, 500, NTFSMAC_CTL_PROC_OK};
	s->procs[1] = (fake_proc){400, 1, 400, NTFSMAC_CTL_PROC_OK};
	s->nprocs = 2;
	s->signed_answer = NTFSMAC_CTL_SYS_YES;
	s->authz_answer = NTFSMAC_CTL_SYS_YES;
	s->right_answer = NTFSMAC_CTL_SYS_YES;
	s->socket_peer = (ntfsmac_ctl_peer){.uid = 501, .pid = 500, .console_user = true,
					    .admin = true, .has_audit_token = true};
	memset(sys, 0, sizeof *sys);
	sys->ctx = s;
	sys->peer_from_fd = fs_peer_from_fd;
	sys->proc_parent = fs_proc_parent;
	sys->client_signed = fs_signed;
	sys->authz_verify = fs_authz;
	sys->right_definition = fs_right;
}

/* A peer that passes the gate and checks 1 and 3. */
static inline ntfsmac_ctl_peer good_peer(void)
{
	ntfsmac_ctl_peer p;

	memset(&p, 0, sizeof p);
	p.uid = 501;
	p.pid = 500;
	p.console_user = true;
	p.admin = true;
	p.has_audit_token = true;
	return p;
}

#define GOOD_AUTH \
	"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

#endif
