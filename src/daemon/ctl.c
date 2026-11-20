/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Control socket of `ntfsmacd` (T-M4-04, docs/ARCHITECTURE.md section 3.4).
 *
 * Two layers:
 *   - ntfsmac_ctl_handle_line(): one request line in, one reply line out.
 *     No socket, no system call of its own; the gate, the parser and the
 *     peer checks of ADR-019 run here, then the operation is called.
 *   - the server: a Unix stream socket, one worker thread, one connection at
 *     a time, with a deadline on every read and write. A peer that fails the
 *     gate is closed at once, before any byte is read, and a client that
 *     sends no byte within a second is dropped, so neither can hold the one
 *     thread for long.
 *
 * Reply lines never contain a newline. The server adds one when it sends.
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "ctl_priv.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE is set on the socket instead */
#endif

/* ------------------------------------------------------------------ */
/* Reply helpers                                                       */
/* ------------------------------------------------------------------ */

/* {"ok":false,"error":"<code>"} or, for a refusal, with ,"reason":"<id>". */
static void put_error(char *reply, size_t cap, ntfsmac_ctl_error e, int refusal)
{
	if (refusal >= 0 && refusal < (int)NTFSMAC_CTL_REFUSE_COUNT_)
		snprintf(reply, cap, "{\"ok\":false,\"error\":\"%s\",\"reason\":\"%s\"}",
			 ntfsmac_ctl_error_name(e),
			 ntfsmac_ctl_refusal_name((ntfsmac_ctl_refusal)refusal));
	else
		snprintf(reply, cap, "{\"ok\":false,\"error\":\"%s\"}", ntfsmac_ctl_error_name(e));
}

/* The status word is one of our fixed names, never a string from the request. */
static void put_failed(char *reply, size_t cap, ntfsmac_dmn_status st)
{
	snprintf(reply, cap, "{\"ok\":false,\"error\":\"%s\",\"status\":\"%s\"}",
		 ntfsmac_ctl_error_name(st == NTFSMAC_DMN_E_NOTIMPL ? NTFSMAC_CTL_ERR_NOT_IMPLEMENTED
								    : NTFSMAC_CTL_ERR_FAILED),
		 ntfsmac_dmn_status_name(st));
}

/* A reply must stay one line: an operation that wrote a newline gets a space instead. */
static void keep_one_line(char *reply, size_t cap)
{
	size_t i;

	reply[cap - 1] = '\0';
	for (i = 0; reply[i] != '\0'; i++)
		if (reply[i] == '\n' || reply[i] == '\r')
			reply[i] = ' ';
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static ntfsmac_dmn_status run_op(const ntfsmac_ctl_ops *ops, void *ctx,
				 const ntfsmac_ctl_peer *peer, const ntfsmac_ctl_request *req,
				 char *reply, size_t cap)
{
	switch (req->cmd) {
	case NTFSMAC_CTL_CMD_LIST:
		return ops->list ? ops->list(ctx, reply, cap) : NTFSMAC_DMN_E_NOTIMPL;
	case NTFSMAC_CTL_CMD_STATUS:
		return ops->status ? ops->status(ctx, req->bsd, reply, cap) : NTFSMAC_DMN_E_NOTIMPL;
	case NTFSMAC_CTL_CMD_MOUNT:
		return ops->mount ? ops->mount(ctx, req->bsd, req->read_only, reply, cap)
				  : NTFSMAC_DMN_E_NOTIMPL;
	case NTFSMAC_CTL_CMD_UNMOUNT:
		return ops->unmount ? ops->unmount(ctx, req->bsd, req->force, reply, cap)
				    : NTFSMAC_DMN_E_NOTIMPL;
	case NTFSMAC_CTL_CMD_DOCTOR:
		return ops->doctor ? ops->doctor(ctx, reply, cap) : NTFSMAC_DMN_E_NOTIMPL;
	case NTFSMAC_CTL_CMD_JOURNAL_CLEAR:
		return ops->journal_clear
			       ? ops->journal_clear(ctx, peer, req->bsd, req->confirm_label,
						    req->confirm_serial, reply, cap)
			       : NTFSMAC_DMN_E_NOTIMPL;
	case NTFSMAC_CTL_CMD_REPAIR:
		return ops->repair
			       ? ops->repair(ctx, peer, req->bsd,
					     req->action == NTFSMAC_CTL_REPAIR_CLEAR_DIRTY
						     ? "clear_dirty"
						     : "sync_mirror",
					     req->confirm_label, req->confirm_serial, reply, cap)
			       : NTFSMAC_DMN_E_NOTIMPL;
	default:
		return NTFSMAC_DMN_E_INVAL;
	}
}

static bool is_privileged(ntfsmac_ctl_cmd c)
{
	return c == NTFSMAC_CTL_CMD_JOURNAL_CLEAR || c == NTFSMAC_CTL_CMD_REPAIR;
}

ntfsmac_dmn_status ntfsmac_ctl_handle_line_env(const ntfsmac_ctl_env *env,
					       const ntfsmac_ctl_ops *ops, void *ctx,
					       const ntfsmac_ctl_peer *peer, const char *line,
					       size_t len, char *reply, size_t cap)
{
	ntfsmac_ctl_request req;
	ntfsmac_ctl_parse_result pr;
	ntfsmac_ctl_refusal refusal;
	ntfsmac_dmn_status st;

	if (reply == NULL || cap < NTFSMAC_CTL_REPLY_MIN)
		return NTFSMAC_DMN_E_INVAL;
	reply[0] = '\0';
	if (env == NULL || ops == NULL || peer == NULL || line == NULL) {
		put_error(reply, cap, NTFSMAC_CTL_ERR_INTERNAL, -1);
		return NTFSMAC_DMN_E_INVAL;
	}

	/* Gate: who may talk to the daemon at all. Before any parsing. */
	refusal = ntfsmac_ctl_check_peer_allowed(peer);
	if (refusal != NTFSMAC_CTL_PASS) {
		put_error(reply, cap, NTFSMAC_CTL_ERR_DENIED, (int)refusal);
		return NTFSMAC_DMN_E_DENIED;
	}

	pr = ntfsmac_ctl_parse_request(line, len, &req);
	if (pr != NTFSMAC_CTL_PARSE_OK) {
		ntfsmac_ctl_error e = pr == NTFSMAC_CTL_PARSE_TOO_LONG ? NTFSMAC_CTL_ERR_LINE_TOO_LONG
				    : pr == NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND
					    ? NTFSMAC_CTL_ERR_UNKNOWN_COMMAND
				    : pr == NTFSMAC_CTL_PARSE_BAD_FIELD ? NTFSMAC_CTL_ERR_BAD_FIELD
									: NTFSMAC_CTL_ERR_BAD_JSON;

		put_error(reply, cap, e, -1);
		return NTFSMAC_DMN_E_INVAL;
	}

	/*
	 * The two channels serve disjoint sets (ADR-019 amendment 2026-11-16): the socket knows
	 * only the uid of its peer, so it never serves journal_clear and repair; the XPC service
	 * serves nothing else.
	 */
	if (is_privileged(req.cmd) != (env->channel == NTFSMAC_CTL_CHAN_XPC)) {
		put_error(reply, cap, NTFSMAC_CTL_ERR_DENIED, (int)NTFSMAC_CTL_REFUSE_WRONG_CHANNEL);
		return NTFSMAC_DMN_E_DENIED;
	}

	/* journal_clear and repair: checks 1 to 4 of ADR-019, before the operation. */
	if (is_privileged(req.cmd)) {
		refusal = ntfsmac_ctl_authorise_privileged(env, ops, ctx, peer, &req);
		if (refusal != NTFSMAC_CTL_PASS) {
			put_error(reply, cap, NTFSMAC_CTL_ERR_DENIED, (int)refusal);
			return NTFSMAC_DMN_E_DENIED;
		}
	}

	st = run_op(ops, ctx, peer, &req, reply, cap);
	keep_one_line(reply, cap);
	if (st == NTFSMAC_DMN_E_NOTIMPL) {
		put_failed(reply, cap, st); /* an operation that is not there has no reply of its own */
	} else if (reply[0] == '\0') {
		if (st == NTFSMAC_DMN_OK)
			snprintf(reply, cap, "{\"ok\":true}");
		else
			put_failed(reply, cap, st);
	}
	return st;
}

/* Process-wide environment for the public entry points. */
static ntfsmac_ctl_replay g_replay;
static pthread_once_t g_replay_once = PTHREAD_ONCE_INIT;

static void replay_once(void)
{
	ntfsmac_ctl_replay_init(&g_replay);
}

static ntfsmac_ctl_env default_env(void)
{
	ntfsmac_ctl_env e;

	pthread_once(&g_replay_once, replay_once);
	e.sys = ntfsmac_ctl_sys_default();
	e.replay = &g_replay;
	e.authz = ntfsmac_ctl_default_authz();
	e.channel = NTFSMAC_CTL_CHAN_SOCKET;
	e.before_unlink = NULL;
	e.before_unlink_ctx = NULL;
	return e;
}

/* The default environment of the other door: the XPC listener's, with the same replay table. */
ntfsmac_ctl_env ntfsmac_ctl_default_env_xpc(void)
{
	ntfsmac_ctl_env e = default_env();

	e.channel = NTFSMAC_CTL_CHAN_XPC;
	return e;
}

ntfsmac_dmn_status ntfsmac_ctl_handle_line(const ntfsmac_ctl_ops *ops, void *ctx,
					   const ntfsmac_ctl_peer *peer, const char *line,
					   size_t len, char *reply, size_t cap)
{
	ntfsmac_ctl_env env = default_env();

	return ntfsmac_ctl_handle_line_env(&env, ops, ctx, peer, line, len, reply, cap);
}

/* ------------------------------------------------------------------ */
/* Server                                                              */
/* ------------------------------------------------------------------ */

#define CONN_DEADLINE_MS 5000 /* total time one connection may take to send its requests */
#define FIRST_BYTE_MS 1000    /* time a connection may stay silent before its first byte */
#define CONN_MAX_REQUESTS 8
#define LISTEN_BACKLOG 8

struct ntfsmac_ctl {
	int listen_fd;
	int wake[2]; /* self-pipe: a byte on wake[1] ends the thread */
	pthread_t thread;
	bool thread_started;
	bool bound; /* our own bind() made the file at `path` */
	bool bound_id; /* bound_dev and bound_ino are those of the file our bind() made */
	dev_t bound_dev;
	ino_t bound_ino;
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	const ntfsmac_ctl_ops *ops;
	void *ctx;
	ntfsmac_ctl_env env;
	char reply[NTFSMAC_CTL_REPLY_MAX];
};

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool wait_fd(int fd, short events, long long deadline)
{
	long long left = deadline - now_ms();
	struct pollfd p;
	int r;

	if (left <= 0)
		return false;
	p.fd = fd;
	p.events = events;
	p.revents = 0;
	do
		r = poll(&p, 1, (int)left);
	while (r < 0 && errno == EINTR);
	return r > 0;
}

static bool send_all(int fd, const char *buf, size_t len, long long deadline)
{
	while (len > 0) {
		ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);

		if (n > 0) {
			buf += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (!wait_fd(fd, POLLOUT, deadline))
				return false;
			continue;
		}
		return false;
	}
	return true;
}

static bool send_reply(struct ntfsmac_ctl *c, int fd, long long deadline)
{
	size_t n = strlen(c->reply);

	c->reply[n] = '\n'; /* room: handle_line leaves at least one byte after the text */
	return send_all(fd, c->reply, n + 1, deadline);
}

static void serve_connection(struct ntfsmac_ctl *c, int fd)
{
	ntfsmac_ctl_peer peer;
	char buf[NTFSMAC_CTL_LINE_MAX + 1];
	size_t have = 0;
	long long deadline = now_ms() + CONN_DEADLINE_MS;
	long long first_deadline = now_ms() + FIRST_BYTE_MS;
	int requests = 0;
	bool eof = false;
	bool got_byte = false;

	if (c->env.sys->peer_from_fd(c->env.sys->ctx, fd, &peer) != 0)
		return; /* cannot identify the peer: close without a word */
	/*
	 * Who may talk to the daemon at all. A peer that fails is closed now,
	 * before any byte is read or sent: it costs the one thread nothing, and
	 * it learns nothing (handle_line applies the same gate again).
	 */
	if (ntfsmac_ctl_check_peer_allowed(&peer) != NTFSMAC_CTL_PASS)
		return;

	while (requests < CONN_MAX_REQUESTS) {
		/* cppcheck-suppress legacyUninitvar ; false positive: `have` bytes are valid, 0 reads nothing */
		char *nl = memchr(buf, '\n', have);
		size_t linelen;

		if (nl == NULL && have == sizeof buf) {
			put_error(c->reply, sizeof c->reply, NTFSMAC_CTL_ERR_LINE_TOO_LONG, -1);
			send_reply(c, fd, deadline);
			return; /* the rest of the stream cannot be trusted to be framed */
		}
		if (nl == NULL && !eof) {
			ssize_t n;

			/* Before the first byte only a short wait is allowed. */
			if (!wait_fd(fd, POLLIN, got_byte ? deadline : first_deadline))
				return; /* idle or slow client */
			n = recv(fd, buf + have, sizeof buf - have, 0);
			if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
				continue;
			if (n < 0)
				return;
			if (n == 0) {
				eof = true;
			} else {
				have += (size_t)n;
				got_byte = true;
			}
			continue;
		}
		if (nl == NULL) {
			if (have == 0)
				return; /* clean end of the stream */
			linelen = have; /* last request without a newline */
		} else {
			linelen = (size_t)(nl - buf);
		}

		(void)ntfsmac_ctl_handle_line_env(&c->env, c->ops, c->ctx, &peer, buf, linelen,
						  c->reply, sizeof c->reply - 1);
		if (!send_reply(c, fd, deadline))
			return;
		requests++;

		if (nl == NULL)
			return;
		have -= linelen + 1;
		memmove(buf, buf + linelen + 1, have);
	}
}

static void *server_main(void *arg)
{
	struct ntfsmac_ctl *c = arg;

	for (;;) {
		struct pollfd p[2];
		int fd, r;

		p[0].fd = c->listen_fd;
		p[0].events = POLLIN;
		p[0].revents = 0;
		p[1].fd = c->wake[0];
		p[1].events = POLLIN;
		p[1].revents = 0;
		do
			r = poll(p, 2, -1);
		while (r < 0 && errno == EINTR);
		if (r < 0 || p[1].revents != 0)
			break;
		if (!(p[0].revents & POLLIN))
			continue;
		fd = accept(c->listen_fd, NULL, NULL);
		if (fd < 0)
			continue;
		(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
		(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#if defined(SO_NOSIGPIPE)
		{
			int one = 1;

			(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
		}
#endif
		serve_connection(c, fd);
		close(fd);
	}
	return NULL;
}

/*
 * Is a daemon listening on the socket file at `path`? A refused connection
 * means the file is left over from a crash. Any other answer (a connection,
 * a full backlog, a permission error) means "do not touch it".
 */
static bool socket_in_use(const struct sockaddr_un *sa)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	bool in_use = true;

	if (fd < 0)
		return true;
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	if (connect(fd, (const struct sockaddr *)sa, sizeof *sa) != 0 && errno == ECONNREFUSED)
		in_use = false;
	close(fd);
	return in_use;
}

/*
 * The stale-socket decision, pure (no system call): `want_*` is the device and
 * inode of the socket that bind_socket() examined and found stale; `now` is the
 * result of a second lstat() of the same path, taken just before the unlink.
 *   - second lstat() failed with ENOENT: the name is gone, so there is nothing to
 *     remove; bind() may proceed (NTFSMAC_CTL_STALE_GONE). bind() fails by itself
 *     if something appeared in the meantime.
 *   - second lstat() failed for any other reason: refuse (fail closed).
 *   - not a socket any more, or another device or inode: the name was replaced
 *     after it was examined; refuse, and the caller leaves the path alone.
 *   - same device, same inode, still a socket: remove it.
 * This pins the decision to the object that was examined. It does not close the
 * window between this second lstat() and the unlink(): unlink() works on a name,
 * and POSIX has no way to unlink "this inode". The directory is root-owned, so
 * only root can race it.
 */
ntfsmac_ctl_stale_action ntfsmac_ctl_stale_socket_decide(dev_t want_dev, ino_t want_ino,
							 const ntfsmac_ctl_stale_stat *now)
{
	if (now == NULL)
		return NTFSMAC_CTL_STALE_REFUSE;
	if (!now->ok)
		return now->err == ENOENT ? NTFSMAC_CTL_STALE_GONE : NTFSMAC_CTL_STALE_REFUSE;
	if (!S_ISSOCK(now->mode) || now->dev != want_dev || now->ino != want_ino)
		return NTFSMAC_CTL_STALE_REFUSE;
	return NTFSMAC_CTL_STALE_REMOVE;
}

/* lstat() the path into the plain-data form the pure decision takes. */
static ntfsmac_ctl_stale_stat stat_path(const char *path)
{
	ntfsmac_ctl_stale_stat now;
	struct stat st;

	memset(&now, 0, sizeof now);
	now.ok = lstat(path, &st) == 0;
	if (now.ok) {
		now.dev = st.st_dev;
		now.ino = st.st_ino;
		now.mode = st.st_mode;
	} else {
		now.err = errno;
	}
	return now;
}

/*
 * Remove the socket file that our own bind() made, and only that one: the path must still
 * name the same socket inode that was recorded right after bind(). If the name was replaced
 * the replacement is left alone. The window between the lstat() and the unlink() stays (see
 * ntfsmac_ctl_stale_socket_decide()).
 */
static void unlink_own_socket(const struct ntfsmac_ctl *c)
{
	ntfsmac_ctl_stale_stat now;

	if (!c->bound || !c->bound_id)
		return; /* not our file, or its identity is unknown: fail closed */
	now = stat_path(c->path);
	if (ntfsmac_ctl_stale_socket_decide(c->bound_dev, c->bound_ino, &now) ==
	    NTFSMAC_CTL_STALE_REMOVE)
		(void)unlink(c->path);
}

/* Bind with mode 0660 and, as root, group staff (section 3.4). */
static ntfsmac_dmn_status bind_socket(struct ntfsmac_ctl *c)
{
	struct sockaddr_un sa;
	struct stat st;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	memcpy(sa.sun_path, c->path, strlen(c->path) + 1);

	if (lstat(c->path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode))
			return NTFSMAC_DMN_E_IO; /* never remove something that is not our socket */
		if (socket_in_use(&sa))
			return NTFSMAC_DMN_E_BUSY; /* another daemon is running: do not take over */
		/* Pin the decision to the inode that was examined (M4 F-06). */
		{
			ntfsmac_ctl_stale_stat now;

			if (c->env.before_unlink != NULL)
				c->env.before_unlink(c->env.before_unlink_ctx, c->path);
			now = stat_path(c->path);
			switch (ntfsmac_ctl_stale_socket_decide(st.st_dev, st.st_ino, &now)) {
			case NTFSMAC_CTL_STALE_REMOVE:
				if (unlink(c->path) != 0)
					return NTFSMAC_DMN_E_IO;
				break;
			case NTFSMAC_CTL_STALE_GONE:
				break; /* already gone: nothing to remove, bind() follows */
			case NTFSMAC_CTL_STALE_REFUSE:
			default:
				if (!now.ok)
					fprintf(stderr,
						"ntfsmacd: %s cannot be examined again (%s); not starting, "
						"path left untouched\n",
						c->path, strerror(now.err));
				else
					fprintf(stderr,
						"ntfsmacd: %s was replaced (different inode or type) while "
						"it was examined; not starting, path left untouched\n",
						c->path);
				return NTFSMAC_DMN_E_BUSY;
			}
		}
	}
	c->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (c->listen_fd < 0)
		return NTFSMAC_DMN_E_IO;
	(void)fcntl(c->listen_fd, F_SETFD, FD_CLOEXEC);
	(void)fcntl(c->listen_fd, F_SETFL, fcntl(c->listen_fd, F_GETFL) | O_NONBLOCK);

	/*
	 * No umask change: it is process wide, and other threads may be running.
	 * It is not needed either. A socket that is bound but not yet listening
	 * refuses every connect(), so nobody can connect while the file still has
	 * the mode the umask gave it. The owner and the mode are set first and
	 * listen() comes last; chown comes before chmod so that the group never
	 * gets access under a wrong owner.
	 */
	if (bind(c->listen_fd, (struct sockaddr *)&sa, sizeof sa) != 0)
		return NTFSMAC_DMN_E_IO;
	c->bound = true;
	{
		/* the identity of the file our own bind() made; stop and failure unlink only that */
		ntfsmac_ctl_stale_stat mine = stat_path(c->path);

		if (!mine.ok || !S_ISSOCK(mine.mode))
			return NTFSMAC_DMN_E_IO;
		c->bound_dev = mine.dev;
		c->bound_ino = mine.ino;
		c->bound_id = true;
	}
	if (geteuid() == 0) {
		struct group *gr = getgrnam("staff");

		if (gr == NULL || chown(c->path, 0, gr->gr_gid) != 0)
			return NTFSMAC_DMN_E_IO;
	}
	if (chmod(c->path, 0660) != 0)
		return NTFSMAC_DMN_E_IO;
	if (listen(c->listen_fd, LISTEN_BACKLOG) != 0)
		return NTFSMAC_DMN_E_IO;
	return NTFSMAC_DMN_OK;
}

ntfsmac_dmn_status ntfsmac_ctl_start_env(const char *socket_path, const ntfsmac_ctl_ops *ops,
					 void *ctx, const ntfsmac_ctl_env *env,
					 ntfsmac_ctl **out)
{
	struct ntfsmac_ctl *c;
	ntfsmac_dmn_status st;

	if (out != NULL)
		*out = NULL;
	if (socket_path == NULL || ops == NULL || env == NULL || env->sys == NULL ||
	    env->sys->peer_from_fd == NULL || env->replay == NULL || out == NULL ||
	    strlen(socket_path) >= sizeof c->path)
		return NTFSMAC_DMN_E_INVAL;

	c = calloc(1, sizeof *c);
	if (c == NULL)
		return NTFSMAC_DMN_E_IO;
	c->listen_fd = -1;
	c->wake[0] = c->wake[1] = -1;
	memcpy(c->path, socket_path, strlen(socket_path) + 1);
	c->ops = ops;
	c->ctx = ctx;
	c->env = *env;
	c->env.channel = NTFSMAC_CTL_CHAN_SOCKET; /* the socket never serves the privileged requests */

	if (pipe(c->wake) != 0) {
		st = NTFSMAC_DMN_E_IO;
		goto fail;
	}
	(void)fcntl(c->wake[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(c->wake[1], F_SETFD, FD_CLOEXEC);
	st = bind_socket(c);
	if (st != NTFSMAC_DMN_OK)
		goto fail;
	if (pthread_create(&c->thread, NULL, server_main, c) != 0) {
		st = NTFSMAC_DMN_E_IO;
		goto fail;
	}
	c->thread_started = true;
	/*
	 * Say it early if the right is not set up: every privileged request is
	 * refused for that reason (each one checks again), the rest still works. Under ROOT_PEER
	 * (check 4 does not use the right) there is nothing to say.
	 */
	if (env->authz == NTFSMAC_CTL_AUTHZ_EXTERNAL_FORM &&
	    (env->sys->right_definition == NULL ||
	     env->sys->right_definition(env->sys->ctx) != NTFSMAC_CTL_SYS_YES))
		fprintf(stderr, "ntfsmacd: the right %s is missing or not as intended; "
				"journal_clear and repair are refused until it is installed\n",
			NTFSMAC_CTL_RIGHT_NAME);
	*out = c;
	return NTFSMAC_DMN_OK;

fail:
	if (c->listen_fd >= 0)
		close(c->listen_fd);
	unlink_own_socket(c); /* never remove a file that our own bind did not make */
	if (c->wake[0] >= 0)
		close(c->wake[0]);
	if (c->wake[1] >= 0)
		close(c->wake[1]);
	free(c);
	return st;
}

ntfsmac_dmn_status ntfsmac_ctl_start(const char *socket_path, const ntfsmac_ctl_ops *ops,
				     void *ctx, ntfsmac_ctl **out)
{
	ntfsmac_ctl_env env = default_env();

	return ntfsmac_ctl_start_env(socket_path, ops, ctx, &env, out);
}

void ntfsmac_ctl_stop(ntfsmac_ctl *c)
{
	char b = 'x';

	if (c == NULL)
		return;
	if (c->thread_started) {
		if (write(c->wake[1], &b, 1) < 0) {
			/* the pipe is ours and open; a failure here would leave join waiting */
			abort();
		}
		pthread_join(c->thread, NULL);
	}
	close(c->listen_fd);
	unlink_own_socket(c); /* a replacement of the file is left alone */
	close(c->wake[0]);
	close(c->wake[1]);
	free(c);
}
