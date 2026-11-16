/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M4-06: driver for the sandbox profiles in a macOS guest (Environment B),
 * run as root by sandbox_vm.sh. It starts the stand-in of sandbox_stub.c
 * through the spawn module, as the daemon starts the probe, volmark and the
 * worker, so the real profiles of integration/sandbox/ are applied by the real
 * code path. Every case runs twice: under the profile, where each check must be
 * refused or allowed as the profile says, and without one (the control), where
 * every check must succeed, so a refusal in the first run can only be the
 * profile.
 *
 *   sandbox_vm SANDBOX_DIR STUB UID GID WORKDIR [CASE [PROBE VOLMARK IMAGE | FUSE]]
 *
 * SANDBOX_DIR  root-owned directory with probe.sb, volmark.sb and worker.sb
 * STUB         the built sandbox_stub
 * UID GID      the console user (the worker's user)
 * WORKDIR      an empty directory, made by the script under /private/tmp
 * PROBE VOLMARK IMAGE   for the case `real`: the built ntfsmac-probe and ntfsmac-volmark and an
 *              NTFS image file (a clean volume; the case sets and clears its dirty flag)
 * FUSE         for the case `fuse2`: the built sandbox_fuse (needs libfuse of macFUSE)
 *
 * Cases (I-SBX-01 to I-SBX-05 and I-SBX-08 to I-SBX-12):
 *   probe        the probe profile
 *   volmark      the volmark profile
 *   worker       the worker profile, usermapping= not set
 *   worker-um    the worker profile with usermapping= set (reads the one file)
 *   desc         a descendant of the worker that calls setpgid(0, 0) and loses its
 *                parent cannot connect() to the control socket (under both spellings of its path)
 *                nor look up the XPC service, with the sandbox's own denial code; after the
 *                checks it runs on, holding the device descriptor, and the poll that reports the
 *                worker's exit kills it (I-SBX-05, I-SBX-12)
 *   desc-broad   the same descendant under the widened profile of `broad`: the last rules are
 *                what refuses it (I-SBX-11)
 *   tree         the worker stays alive with such a descendant; killing the worker's group also
 *                kills the descendant, which the tree walk finds (I-SBX-12)
 *   broad        the worker profile with `(allow network-outbound)` and `(allow mach-lookup)` added
 *                right after `(deny default)`: the control socket, under both spellings of its path
 *                and by a relative path from its directory, and the XPC service stay denied (the
 *                last rules of the profile win), everything else those rules cover is allowed
 *                (I-SBX-08)
 *   fuse2        a worker under the profile asks macFUSE for a second mount, with libfuse, on a
 *                directory that the console user owns and that is not empty, and the case records
 *                what happens (I-SBX-09, residual risk R-07)
 *   fail         the real sandbox-exec with a profile that does not compile, a profile with a
 *                parameter that is not given, and a tool that is missing: the program never
 *                starts (I-SBX-10)
 *   real         the real probe and volmark under their profiles, on the image file and on the
 *                same image attached as a raw disk (ioctl on a device descriptor): the probe
 *                gives the same verdict as without a profile; volmark sets and clears the dirty
 *                flag and the probe sees both (I-SBX-07)
 *
 * What the driver makes in the guest, and removes at the end: a TCP listener on
 * 127.0.0.1, a listening Unix socket at the control socket path and one at another path
 * (both mode 0666), and /usr/local/etc/ntfs-macos/{UserMapping,other} (root, 0644). The
 * Mach service of the privileged requests is registered by the script.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <grp.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include "ntfsmac/ctl_proto.h"
#include "ntfsmac/probe.h"
#include "sandbox_test.h"
#include "spawn_config.h"

static int g_fails;
static uid_t g_uid;
static gid_t g_gid;
static const char *g_sbx_dir, *g_stub, *g_dir;
static int g_tcp = -1, g_ctl = -1, g_other = -1;
static bool g_made_etc, g_made_dir, g_made_um, g_made_other;

#define PASS(...) \
	do { printf("PASS: " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define FAIL(...) \
	do { printf("FAIL: " __VA_ARGS__); printf("\n"); fflush(stdout); g_fails++; } while (0)
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

/* ---------------------------------------------------------------- */
/* The guest set-up                                                  */
/* ---------------------------------------------------------------- */

static int listen_unix(const char *path)
{
	struct sockaddr_un a;
	int s = socket(AF_UNIX, SOCK_STREAM, 0);

	if (s < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || chmod(path, 0666) != 0
	    || listen(s, 128) != 0) {
		close(s);
		return -1;
	}
	return s;
}

static void cleanup(void)
{
	if (g_ctl >= 0)
		(void)unlink(NTFSMAC_CTL_SOCKET_PATH);
	if (g_other >= 0)
		(void)unlink(SBX_OTHER_SOCKET);
	if (g_made_um)
		(void)unlink(NTFSMAC_USERMAPPING_PATH);
	if (g_made_other)
		(void)unlink(SBX_OTHER_FILE);
	if (g_made_dir)
		(void)rmdir(SBX_USERMAPPING_DIR);
	if (g_made_etc)
		(void)rmdir("/usr/local/etc");
}

static void on_signal(int sig)
{
	(void)sig;
	cleanup();
	_exit(130);
}

static int make_file(const char *path, mode_t mode, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);

	if (fd < 0)
		return -1;
	if (write(fd, text, strlen(text)) != (ssize_t)strlen(text)) {
		close(fd);
		return -1;
	}
	(void)fchmod(fd, mode);
	return close(fd);
}

static int lookup_service(const char *name)
{
	mach_port_t p = MACH_PORT_NULL;
	kern_return_t kr;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	kr = bootstrap_look_up(bootstrap_port, name, &p);
#pragma clang diagnostic pop
	if (kr == KERN_SUCCESS)
		mach_port_deallocate(mach_task_self(), p);
	return (int)kr;
}

/* 0 on success. Refuses to run where the real daemon's socket or files already exist. */
static int setup_guest(void)
{
	struct sockaddr_in a;
	int one = 1, kr;

	if (access(NTFSMAC_CTL_SOCKET_PATH, F_OK) == 0 || access(NTFSMAC_USERMAPPING_PATH, F_OK) == 0
	    || access(SBX_OTHER_FILE, F_OK) == 0) {
		fprintf(stderr, "sandbox_vm: the control socket or a UserMapping file exists; not a disposable guest\n");
		return -1;
	}
	if (mkdir("/usr/local/etc", 0755) == 0)
		g_made_etc = true;
	if (mkdir(SBX_USERMAPPING_DIR, 0755) == 0) {
		g_made_dir = true;
	} else if (errno != EEXIST) {
		fprintf(stderr, "sandbox_vm: mkdir %s: %s\n", SBX_USERMAPPING_DIR, strerror(errno));
		return -1;
	}
	if (make_file(NTFSMAC_USERMAPPING_PATH, 0644, "# test mapping\n") != 0)
		return -1;
	g_made_um = true;
	if (make_file(SBX_OTHER_FILE, 0644, "other\n") != 0)
		return -1;
	g_made_other = true;

	g_tcp = socket(AF_INET, SOCK_STREAM, 0);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(SBX_TCP_PORT);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	(void)setsockopt(g_tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (g_tcp < 0 || bind(g_tcp, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(g_tcp, 128) != 0) {
		fprintf(stderr, "sandbox_vm: TCP listener: %s\n", strerror(errno));
		return -1;
	}
	(void)unlink(SBX_OTHER_SOCKET);
	g_ctl = listen_unix(NTFSMAC_CTL_SOCKET_PATH);
	g_other = listen_unix(SBX_OTHER_SOCKET);
	if (g_ctl < 0 || g_other < 0) {
		fprintf(stderr, "sandbox_vm: Unix listeners: %s\n", strerror(errno));
		return -1;
	}
	kr = lookup_service(NTFSMAC_CTL_XPC_SERVICE);
	if (kr != 0) {
		fprintf(stderr, "sandbox_vm: the Mach service %s is not registered (lookup %d); "
			"sandbox_vm.sh registers it\n", NTFSMAC_CTL_XPC_SERVICE, kr);
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------- */
/* Running one stand-in                                              */
/* ---------------------------------------------------------------- */

static ntfsmac_dmn_status fake_lookup(const char *mount_point, bool *found, bool *read_only)
{
	(void)mount_point;
	*found = true;
	*read_only = false;
	return NTFSMAC_DMN_OK;
}

/* A new log file per run; the child's stderr is this descriptor. */
static int open_log(const char *name, char *path, size_t cap)
{
	snprintf(path, cap, "%s/%s.log", g_dir, name);
	return open(path, O_RDWR | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0666);
}

static int count_prefix(const char *path, const char *prefix, bool echo)
{
	char line[300];
	FILE *f = fopen(path, "r");
	int n = 0;

	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, prefix, strlen(prefix)) == 0)
			n++;
		if (echo && (strncmp(line, "PASS: ", 6) == 0 || strncmp(line, "FAIL: ", 6) == 0))
			printf("  stub %s", line);
	}
	fclose(f);
	return n;
}

static bool log_has(const char *path, const char *text)
{
	char line[300];
	FILE *f = fopen(path, "r");
	bool found = false;

	if (f == NULL)
		return false;
	while (!found && fgets(line, sizeof(line), f) != NULL)
		found = strstr(line, text) != NULL;
	fclose(f);
	return found;
}

/* A program is made under its mode name by a hard link, so the profile sees a regular file. */
static const char *link_as(const char *src, const char *mode, char *buf, size_t cap)
{
	snprintf(buf, cap, "%s/%s", g_dir, mode);
	(void)unlink(buf);
	if (link(src, buf) != 0) {
		FAIL("link %s: %s", buf, strerror(errno));
		return NULL;
	}
	return buf;
}

static const char *stub_path(const char *mode, char *buf, size_t cap)
{
	return link_as(g_stub, mode, buf, cap);
}

static ntfsmac_spawn_config config_for(bool sandboxed, int log_fd)
{
	ntfsmac_spawn_config c;

	ntfsmac_spawn_config_default(&c);
	c.sandbox_dir = sandboxed ? g_sbx_dir : NULL;
	c.child_stderr_fd = log_fd;
	c.mount_poll_ms = 10;
	c.mount_lookup = fake_lookup;
	return c;
}

static int make_device(void)
{
	char path[300];
	int fd;

	snprintf(path, sizeof(path), "%s/device", g_dir);
	/* Readable by every user: the control run must be able to open /dev/fd/N as `nobody`. */
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0 || write(fd, "ABCDEFGH", 8) != 8 || fchmod(fd, 0644) != 0) {
		FAIL("cannot make the stand-in device");
		return -1;
	}
	return fd;
}

/* The checks of one run: no FAIL line in the log, and enough PASS lines to show it ran. */
static void judge(const char *label, const char *log, int min_pass)
{
	int pass = count_prefix(log, "PASS: ", true);
	int fail = count_prefix(log, "FAIL: ", false);

	CHECK(fail == 0, "%s: no check failed (%d failed)", label, fail);
	CHECK(pass >= min_pass, "%s: %d checks passed (at least %d expected)", label, pass, min_pass);
}

static void case_probe_or_volmark(bool volmark)
{
	const char *role = volmark ? "volmark" : "probe";
	int pass_no;

	printf("== %s: the %s profile (I-SBX-%s)\n", role, role, volmark ? "02" : "01");
	for (pass_no = 0; pass_no < 2; pass_no++) {
		bool sbx = pass_no == 0;
		char mode[40], path[300], logp[300], label[64];
		const char *prog;
		ntfsmac_spawn_config c;
		ntfsmac_probe_run pr;
		ntfsmac_probe_verdict v;
		ntfsmac_volmark_run vr;
		ntfsmac_dmn_status st;
		int fd, logfd, code = 0;

		snprintf(mode, sizeof(mode), "%s-%s", role, sbx ? "sbx" : "ctl");
		snprintf(label, sizeof(label), "%s %s", role, sbx ? "under the profile" : "control, no profile");
		prog = stub_path(mode, path, sizeof(path));
		fd = make_device();
		logfd = open_log(mode, logp, sizeof(logp));
		if (prog == NULL || fd < 0 || logfd < 0) {
			FAIL("%s: set-up", label);
			continue;
		}
		c = config_for(sbx, logfd);
		if (volmark)
			c.volmark_path = prog;
		else
			c.probe_path = prog;
		ntfsmac_spawn_config_set(&c);
		if (volmark) {
			vr.device_fd = fd;
			vr.serial = 0x12AB;
			vr.total_sectors = 2048;
			vr.timeout_s = 30;
			st = ntfsmac_spawn_volmark(&vr, true, &code);
		} else {
			pr.device_fd = fd;
			pr.timeout_s = 30;
			st = ntfsmac_spawn_probe(&pr, &v);
		}
		CHECK(st == NTFSMAC_DMN_OK, "%s: the stand-in ran and all its checks held (status %s, code %d)",
		      label, ntfsmac_dmn_status_name(st), code);
		judge(label, logp, 14);
		close(fd);
		close(logfd);
	}
}

/* Wait for the worker stand-in to exit; its status is the verdict of its own checks. */
static int wait_exit(ntfsmac_child *child)
{
	bool exited = false;
	int status = -1, i;

	for (i = 0; i < 400 && !exited; i++) {
		if (ntfsmac_child_poll(child, &exited, &status) != NTFSMAC_DMN_OK)
			break;
		if (!exited)
			sleep_ms(50);
	}
	if (!exited) {
		(void)ntfsmac_child_kill_group(child);
		return -1;
	}
	return status;
}

/*
 * worker.sb with two broad allows right after `(deny default)`, in a directory of its own. The
 * profile's own last rules must still win over them.
 */
static const char *broad_dir(void)
{
	static char dir[320];
	char src[320], dst[340], line[512];
	FILE *in, *out;

	snprintf(dir, sizeof(dir), "%s/broad", g_dir);
	(void)mkdir(dir, 0755);
	snprintf(src, sizeof(src), "%s/worker.sb", g_sbx_dir);
	snprintf(dst, sizeof(dst), "%s/worker.sb", dir);
	in = fopen(src, "r");
	out = fopen(dst, "w");
	if (in == NULL || out == NULL) {
		FAIL("cannot make the widened profile");
		return dir;
	}
	while (fgets(line, sizeof(line), in) != NULL) {
		fputs(line, out);
		if (strncmp(line, "(deny default)", 14) == 0)
			fputs("(allow network-outbound)\n(allow mach-lookup)\n", out);
	}
	fclose(in);
	fclose(out);
	(void)chmod(dst, 0644);
	return dir;
}

/* The pid the descendant reported ("DESC-PID n"), 0 if none. */
static pid_t desc_pid(const char *path)
{
	char line[300];
	FILE *f = fopen(path, "r");
	pid_t pid = 0;
	int n;

	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL)
		if (sscanf(line, "DESC-PID %d", &n) == 1)
			pid = (pid_t)n;
	fclose(f);
	return pid;
}

/* Is the process gone, within `ms`? A zombie of someone else's is not there to wait for: init reaps it. */
static bool pid_gone(pid_t pid, unsigned ms)
{
	uint64_t end = now_ms() + ms;

	for (;;) {
		if (kill(pid, 0) != 0 && errno == ESRCH)
			return true;
		if (now_ms() >= end)
			return false;
		sleep_ms(50);
	}
}

typedef struct worker_opts {
	bool sbx;     /* under the profile (else the control run) */
	bool um;      /* usermapping= set */
	bool desc;    /* a descendant that outlives the worker */
	bool broad;   /* the widened profile */
	bool tree;    /* the worker stays alive with the descendant; the driver kills the group */
	bool cwd;     /* start in the directory of the control socket, to try a relative path */
} worker_opts;

/* Take and close the connections that earlier runs left queued, so that the queues never fill. */
static void drain_listeners(void)
{
	int fds[3] = { g_tcp, g_ctl, g_other }, i, c;

	for (i = 0; i < 3; i++) {
		(void)fcntl(fds[i], F_SETFL, O_NONBLOCK);
		while ((c = accept(fds[i], NULL, NULL)) >= 0)
			close(c);
	}
}

static void run_worker(worker_opts o)
{
	char mode[60], path[300], logp[300], label[160];
	const char *prog;
	ntfsmac_spawn_config c;
	ntfsmac_worker_run run;
	ntfsmac_child child;
	ntfsmac_dmn_status st;
	bool ro = false;
	int fd, logfd, status;
	pid_t dpid = 0;

	drain_listeners();
	snprintf(mode, sizeof(mode), "worker-%s%s%s%s%s%s", o.sbx ? "sbx" : "ctl", o.um ? "-um" : "",
		 o.desc ? "-desc" : "", o.tree ? "-tree" : "", o.broad ? "-broad" : "", o.cwd ? "-cwd" : "");
	snprintf(label, sizeof(label), "worker %s%s%s%s%s%s", o.sbx ? "under the profile" : "control, no profile",
		 o.um ? ", usermapping=" : "", o.desc ? ", with a descendant" : "",
		 o.tree ? ", with a descendant and the worker alive" : "",
		 o.broad ? " widened ahead of its last rules" : "", o.cwd ? ", started in /var/run" : "");
	prog = stub_path(mode, path, sizeof(path));
	fd = make_device();
	logfd = open_log(mode, logp, sizeof(logp));
	if (prog == NULL || fd < 0 || logfd < 0) {
		FAIL("%s: set-up", label);
		return;
	}
	c = config_for(o.sbx, logfd);
	if (o.broad)
		c.sandbox_dir = broad_dir();
	c.worker_path = prog;
	c.usermapping = o.um;
	ntfsmac_spawn_config_set(&c);

	memset(&run, 0, sizeof(run));
	run.device_fd = fd;
	run.errfd_write = -1;
	run.mount_point = "/Volumes/sbxstub";
	run.read_only = false;
	run.console_uid = g_uid;
	run.console_gid = g_gid;
	run.mount_wait_s = 30;
	if (o.cwd && chdir("/var/run") != 0)
		FAIL("%s: chdir /var/run: %s", label, strerror(errno));
	st = ntfsmac_spawn_worker(&run, &child, &ro);
	if (o.cwd && chdir(g_dir) != 0)
		FAIL("chdir %s: %s", g_dir, strerror(errno));
	CHECK(st == NTFSMAC_DMN_OK, "%s: the worker started (status %s)", label, ntfsmac_dmn_status_name(st));
	if (st != NTFSMAC_DMN_OK) {
		close(fd);
		close(logfd);
		return;
	}
	if (o.desc || o.tree) {
		/* The descendant does its checks while the worker is a zombie (desc) or alive (tree). */
		uint64_t end = now_ms() + 20000u;

		while (now_ms() < end && !log_has(logp, "DESC-DONE"))
			sleep_ms(100);
		CHECK(log_has(logp, "DESC-DONE"), "%s: the descendant finished its attempts%s", label,
		      o.desc ? " after the worker exited" : "");
		dpid = desc_pid(logp);
		CHECK(dpid > 1 && kill(dpid, 0) == 0, "%s: the descendant still runs (pid %d)", label, (int)dpid);
	}
	if (o.tree) {
		/* The teardown: the worker is alive, so the descendant is found by the tree walk. */
		CHECK(ntfsmac_child_kill_group(&child) == NTFSMAC_DMN_OK, "%s: kill of the worker's group", label);
		status = wait_exit(&child);
		CHECK(status == 128 + SIGKILL, "%s: the worker was killed (exit status %d)", label, status);
	} else {
		status = wait_exit(&child);
		CHECK(status == 0, "%s: the stand-in ran and all its checks held (exit status %d)", label, status);
	}
	if (dpid > 1) {
		unsigned killed = 99;

		CHECK(pid_gone(dpid, 5000), "%s: the descendant is gone after the teardown of the worker", label);
		CHECK(ntfsmac_child_device_check(&child, &killed) == NTFSMAC_DMN_OK,
		      "%s: no process of the user holds the device any more (%u more killed)", label, killed);
	}
	judge(label, logp, o.desc ? 22 : o.tree ? 21 : 17);
	close(fd);
	close(logfd);
}

static void case_worker(bool um)
{
	printf("== worker%s: the worker profile (I-SBX-%s)\n", um ? "-um" : "", um ? "04" : "03");
	run_worker((worker_opts){ .sbx = true, .um = um });
	run_worker((worker_opts){ .um = um });
}

static void case_desc(void)
{
	printf("== desc: a descendant that leaves the process group and loses its parent (I-SBX-05, I-SBX-12)\n");
	run_worker((worker_opts){ .sbx = true, .desc = true });
	run_worker((worker_opts){ .desc = true });
}

static void case_desc_broad(void)
{
	printf("== desc-broad: the same descendant under the widened profile (I-SBX-11)\n");
	run_worker((worker_opts){ .sbx = true, .desc = true, .broad = true });
}

static void case_tree(void)
{
	printf("== tree: the worker group is killed while a descendant outside the group lives (I-SBX-12)\n");
	run_worker((worker_opts){ .sbx = true, .tree = true });
	run_worker((worker_opts){ .tree = true });
}

static void case_broad(void)
{
	printf("== broad: the last rules win over a wider allow (I-SBX-08)\n");
	run_worker((worker_opts){ .sbx = true, .broad = true, .cwd = true });
	run_worker((worker_opts){ .cwd = true });
}

/* ---------------------------------------------------------------- */
/* A second FUSE mount (I-SBX-09)                                    */
/* ---------------------------------------------------------------- */

static const char *g_fuse_bin;

static bool mounted_at(const char *dir, char *fstype, size_t cap, unsigned *flags)
{
	struct statfs *fs;
	int n = getfsstat(NULL, 0, MNT_NOWAIT), i;
	bool found = false;

	if (n <= 0)
		return false;
	fs = calloc((size_t)n + 8, sizeof(*fs));
	if (fs == NULL)
		return false;
	n = getfsstat(fs, (int)(((size_t)n + 8) * sizeof(*fs)), MNT_NOWAIT);
	for (i = 0; i < n; i++) {
		if (strcmp(fs[i].f_mntonname, dir) == 0) {
			found = true;
			snprintf(fstype, cap, "%s", fs[i].f_fstypename);
			*flags = fs[i].f_flags;
		}
	}
	free(fs);
	return found;
}

/* In a child as the console user: read `path`; 0 if it holds `want`. Bounded to 20 s. */
static int read_as_user(const char *path, const char *want)
{
	pid_t p = fork();
	int st;
	uint64_t end;

	if (p < 0)
		return -1;
	if (p == 0) {
		char buf[128];
		gid_t groups[1] = { g_gid };
		int fd;
		ssize_t n;

		if (setgroups(1, groups) != 0 || setgid(g_gid) != 0 || setuid(g_uid) != 0)
			_exit(10);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			_exit(11);
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n < 0)
			_exit(12);
		buf[n] = '\0';
		_exit(strcmp(buf, want) == 0 ? 0 : 13);
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

/* unmount(2) with MNT_FORCE in a child, so that a stuck call cannot stall the driver. */
static void force_unmount(const char *dir)
{
	pid_t p = fork();
	int st, i;

	if (p < 0)
		return;
	if (p == 0)
		_exit(unmount(dir, MNT_FORCE) == 0 ? 0 : 1);
	for (i = 0; i < 250; i++) { /* 5 s */
		if (waitpid(p, &st, WNOHANG) == p)
			return;
		sleep_ms(20);
	}
	kill(p, SIGKILL);
	(void)waitpid(p, &st, 0);
}

#define FUSE2_TEXT "served by the second mount\n"

/*
 * One run of the second-mount stand-in. The directory `victim` is the console user's, it is not
 * empty, and the stand-in is started as the worker is. `*mounted` and `*served` tell what the
 * driver saw from outside: a mount-table entry at the directory, and the stand-in's file read
 * by the console user.
 */
static void run_fuse2(bool sbx, bool *mounted, bool *served)
{
	char mode[40], path[300], logp[300], victim[320], f[340], fstype[32] = "", rd[340];
	const char *prog;
	ntfsmac_spawn_config c;
	ntfsmac_worker_run run;
	ntfsmac_child child;
	ntfsmac_dmn_status st;
	unsigned flags = 0;
	bool ro = false, back;
	int fd, logfd, i, fi;
	uint64_t end;

	*mounted = *served = false;
	snprintf(mode, sizeof(mode), "fuse2-%s", sbx ? "sbx" : "ctl");
	snprintf(victim, sizeof(victim), "%s/victim", g_dir);
	snprintf(f, sizeof(f), "%s/planted.txt", victim);
	snprintf(rd, sizeof(rd), "%s/pwned.txt", victim);
	/* The directory of a user's own, with a file in it: ~/Library/LaunchAgents is such a directory. */
	if (mkdir(victim, 0755) != 0 && errno != EEXIST) {
		FAIL("%s: mkdir %s: %s", mode, victim, strerror(errno));
		return;
	}
	fi = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fi < 0 || write(fi, "planted\n", 8) != 8 || fchown(fi, g_uid, g_gid) != 0 || chown(victim, g_uid, g_gid) != 0) {
		FAIL("%s: cannot make the directory of the user", mode);
		if (fi >= 0)
			close(fi);
		return;
	}
	close(fi);
	prog = link_as(g_fuse_bin, mode, path, sizeof(path));
	fd = make_device();
	logfd = open_log(mode, logp, sizeof(logp));
	if (prog == NULL || fd < 0 || logfd < 0) {
		FAIL("%s: set-up", mode);
		return;
	}
	c = config_for(sbx, logfd);
	c.worker_path = prog;
	ntfsmac_spawn_config_set(&c);
	memset(&run, 0, sizeof(run));
	run.device_fd = fd;
	run.errfd_write = -1;
	run.mount_point = "/Volumes/sbxstub";
	run.console_uid = g_uid;
	run.console_gid = g_gid;
	run.mount_wait_s = 30;
	st = ntfsmac_spawn_worker(&run, &child, &ro);
	CHECK(st == NTFSMAC_DMN_OK, "%s: the stand-in started (status %s)", mode, ntfsmac_dmn_status_name(st));
	if (st == NTFSMAC_DMN_OK) {
		end = now_ms() + 30000u;
		while (now_ms() < end && !log_has(logp, "FUSE2-RETURN")) {
			if (mounted_at(victim, fstype, sizeof(fstype), &flags) && log_has(logp, "FUSE2-SERVING"))
				break;
			sleep_ms(100);
		}
		*mounted = mounted_at(victim, fstype, sizeof(fstype), &flags);
		*served = *mounted && read_as_user(rd, FUSE2_TEXT) == 0;
		printf("  %s: %s; mount table %s (type %s, flags 0x%x); the user reads %s: %s\n", mode,
		       log_has(logp, "FUSE2-RETURN") ? "libfuse returned at once" : "libfuse is serving",
		       *mounted ? "has an entry at the directory" : "has no entry at the directory", fstype, flags,
		       "pwned.txt", *served ? "the stand-in's file" : "nothing of it");
		if (log_has(logp, "FUSE2-RETURN")) {
			char cmd[400];

			snprintf(cmd, sizeof(cmd), "sed 's/^/  %s log: /' '%s'", mode, logp);
			fflush(stdout);
			if (system(cmd) != 0) {
				/* only a diagnostic */
			}
		}
		/* Clean up: kill the stand-in (and what it started), take the mount away. */
		(void)ntfsmac_child_kill_group(&child);
		(void)wait_exit(&child);
		for (i = 0; i < 10 && mounted_at(victim, fstype, sizeof(fstype), &flags); i++) {
			force_unmount(victim);
			sleep_ms(200);
		}
		back = !mounted_at(victim, fstype, sizeof(fstype), &flags) && access(f, F_OK) == 0;
		CHECK(back, "%s: clean-up: the mount is gone and the user's own file is visible again", mode);
	}
	close(fd);
	close(logfd);
}

static void case_fuse2(void)
{
	bool mounted, served;

	printf("== fuse2: a second FUSE mount by the worker, on a directory of the console user (I-SBX-09)\n");
	if (g_fuse_bin == NULL) {
		FAIL("fuse2: the case needs the built sandbox_fuse as the 7th argument");
		return;
	}
	run_fuse2(false, &mounted, &served);
	CHECK(mounted && served, "fuse2 control, no profile: the stand-in's second mount appears and serves its file");
	run_fuse2(true, &mounted, &served);
	/*
	 * Recorded, not wished away: the profile lets the worker ask the macFUSE mount service for a
	 * mount, and the service mounts any libfuse file system on a directory that the user owns. The
	 * check passes while that is so. If it ever fails, macFUSE refuses the second mount, and
	 * R-07 and ARCHITECTURE.md section 6 must say so.
	 */
	CHECK(mounted && served, "fuse2 under the worker profile: KNOWN RESIDUAL RISK (R-07), the second mount "
	      "appears on the user's directory and serves the worker's files");
}

/* ---------------------------------------------------------------- */
/* sandbox-exec that cannot apply the profile (I-SBX-10)             */
/* ---------------------------------------------------------------- */

/* A copy of the three profiles in `<g_dir>/<name>`, with `extra` added to probe.sb. */
static const char *profile_dir_with(const char *name, const char *extra)
{
	static char dir[320];
	static const char *files[] = { "probe.sb", "volmark.sb", "worker.sb" };
	char src[340], dst[340], line[512];
	FILE *in, *out;
	size_t i;

	snprintf(dir, sizeof(dir), "%s/%s", g_dir, name);
	(void)mkdir(dir, 0755);
	for (i = 0; i < 3; i++) {
		snprintf(src, sizeof(src), "%s/%s", g_sbx_dir, files[i]);
		snprintf(dst, sizeof(dst), "%s/%s", dir, files[i]);
		in = fopen(src, "r");
		out = fopen(dst, "w");
		if (in == NULL || out == NULL) {
			FAIL("cannot make the profile directory %s", name);
			return dir;
		}
		while (fgets(line, sizeof(line), in) != NULL)
			fputs(line, out);
		if (i == 0 && extra != NULL)
			fputs(extra, out);
		fclose(in);
		fclose(out);
		(void)chmod(dst, 0644);
	}
	return dir;
}

static void probe_attempt(const char *what, const char *dir, const char *tool, bool must_run)
{
	char mode[40] = "probe-sbx", path[300], logp[300];
	const char *prog;
	ntfsmac_spawn_config c;
	ntfsmac_probe_run pr;
	ntfsmac_probe_verdict v;
	ntfsmac_dmn_status st;
	int fd, logfd;

	prog = stub_path(mode, path, sizeof(path));
	fd = make_device();
	logfd = open_log("probe-fail", logp, sizeof(logp));
	if (prog == NULL || fd < 0 || logfd < 0) {
		FAIL("%s: set-up", what);
		return;
	}
	c = config_for(true, logfd);
	c.probe_path = prog;
	c.sandbox_dir = dir;
	if (tool != NULL)
		c.sandbox_exec_path = tool;
	ntfsmac_spawn_config_set(&c);
	pr.device_fd = fd;
	pr.timeout_s = 30;
	st = ntfsmac_spawn_probe(&pr, &v);
	if (must_run) {
		CHECK(st == NTFSMAC_DMN_OK && log_has(logp, "PASS: start"), "%s: the program ran (status %s)", what,
		      ntfsmac_dmn_status_name(st));
	} else {
		CHECK(st == NTFSMAC_DMN_E_IO, "%s: the spawn failed (status %s)", what, ntfsmac_dmn_status_name(st));
		CHECK(!log_has(logp, "PASS: start") && !log_has(logp, "FAIL: "),
		      "%s: the program never started (its first line is missing from its log)", what);
		CHECK(v.state == NTFSMAC_PROBE_STATE_UNKNOWN, "%s: the verdict is unknown", what);
	}
	close(fd);
	close(logfd);
}

static void case_fail(void)
{
	char tool[320];

	printf("== fail: sandbox-exec that cannot apply the profile never starts the program (I-SBX-10)\n");
	probe_attempt("the profile as it is (control)", g_sbx_dir, NULL, true);
	probe_attempt("a profile with a syntax error", profile_dir_with("fail-syntax", "\n;; broken\n(this is not a rule\n"),
		      NULL, false);
	probe_attempt("a profile that names a parameter that is not given",
		      profile_dir_with("fail-param", "\n;; broken\n(allow file-read* (literal (param \"NO_SUCH_PARAM\")))\n"),
		      NULL, false);
	snprintf(tool, sizeof(tool), "%s/no-such-sandbox-exec", g_dir);
	probe_attempt("a sandbox-exec that is missing", g_sbx_dir, tool, false);
}

/* ---------------------------------------------------------------- */
/* The real probe and volmark                                        */
/* ---------------------------------------------------------------- */

static const char *g_probe_bin, *g_volmark_bin, *g_image;

static ntfsmac_dmn_status real_probe(bool sbx, const char *path, int flags, ntfsmac_probe_verdict *v,
				     const char *logname)
{
	char logp[300];
	ntfsmac_spawn_config c;
	ntfsmac_probe_run pr;
	ntfsmac_dmn_status st;
	int fd = open(path, flags | O_CLOEXEC), logfd = open_log(logname, logp, sizeof(logp));

	if (fd < 0 || logfd < 0) {
		FAIL("real probe: cannot open %s: %s", path, strerror(errno));
		ntfsmac_probe_verdict_unknown(v);
		return NTFSMAC_DMN_E_IO;
	}
	c = config_for(sbx, logfd);
	c.probe_path = g_probe_bin;
	ntfsmac_spawn_config_set(&c);
	pr.device_fd = fd;
	pr.timeout_s = 60;
	st = ntfsmac_spawn_probe(&pr, v);
	close(fd);
	close(logfd);
	if (count_prefix(logp, "", false) > 0)
		printf("  probe stderr: see %s\n", logp);
	return st;
}

static int real_volmark(const char *path, bool set, const ntfsmac_probe_verdict *v, const char *logname)
{
	char logp[300];
	ntfsmac_spawn_config c;
	ntfsmac_volmark_run vr;
	ntfsmac_dmn_status st;
	int fd = open(path, O_RDWR | O_CLOEXEC), logfd = open_log(logname, logp, sizeof(logp)), code = -9;

	if (fd < 0 || logfd < 0) {
		FAIL("real volmark: cannot open %s: %s", path, strerror(errno));
		return -9;
	}
	c = config_for(true, logfd);
	c.volmark_path = g_volmark_bin;
	ntfsmac_spawn_config_set(&c);
	vr.device_fd = fd;
	vr.serial = v->serial;
	vr.total_sectors = v->total_sectors;
	vr.timeout_s = 60;
	st = ntfsmac_spawn_volmark(&vr, set, &code);
	close(fd);
	close(logfd);
	if (st != NTFSMAC_DMN_OK)
		printf("  volmark %s: status %s, code %d\n", set ? "set" : "clear", ntfsmac_dmn_status_name(st), code);
	/* A refused DKIOCSYNCHRONIZE is a warning on stderr, not a failure: it must not appear. */
	CHECK(!log_has(logp, "warning"), "volmark %s printed no warning (a refused ioctl would show here)",
	      set ? "set" : "clear");
	if (log_has(logp, "warning") || st != NTFSMAC_DMN_OK) {
		char cmd[400];

		snprintf(cmd, sizeof(cmd), "sed 's/^/  volmark log: /' '%s'", logp);
		fflush(stdout);
		if (system(cmd) != 0) {
			/* only a diagnostic */
		}
	}
	return st == NTFSMAC_DMN_OK ? 0 : (code != 0 ? code : -9);
}

static void real_on(const char *what, const char *rpath, const char *wpath)
{
	ntfsmac_probe_verdict vs, vc, v2;
	ntfsmac_dmn_status st;
	int rc;

	st = real_probe(false, rpath, O_RDONLY, &vc, "real-probe-ctl");
	CHECK(st == NTFSMAC_DMN_OK && vc.state == NTFSMAC_PROBE_STATE_CLEAN,
	      "%s: control probe without a profile: state %s (status %s)", what,
	      ntfsmac_probe_state_name(vc.state), ntfsmac_dmn_status_name(st));
	st = real_probe(true, rpath, O_RDONLY, &vs, "real-probe-sbx");
	CHECK(st == NTFSMAC_DMN_OK, "%s: the probe runs under probe.sb (status %s)", what,
	      ntfsmac_dmn_status_name(st));
	CHECK(vs.state == vc.state && vs.serial == vc.serial && vs.total_sectors == vc.total_sectors,
	      "%s: the verdict under the profile equals the control (state %s)", what,
	      ntfsmac_probe_state_name(vs.state));
	if (st != NTFSMAC_DMN_OK || vs.total_sectors == 0)
		return;
	rc = real_volmark(wpath, true, &vs, "real-volmark-set");
	CHECK(rc == 0, "%s: volmark set runs under volmark.sb (code %d)", what, rc);
	st = real_probe(true, rpath, O_RDONLY, &v2, "real-probe-sbx2");
	CHECK(st == NTFSMAC_DMN_OK && v2.state == NTFSMAC_PROBE_STATE_DIRTY_FLAG,
	      "%s: the probe sees the flag set by volmark (state %s)", what, ntfsmac_probe_state_name(v2.state));
	rc = real_volmark(wpath, false, &vs, "real-volmark-clear");
	CHECK(rc == 0, "%s: volmark clear runs under volmark.sb (code %d)", what, rc);
	st = real_probe(true, rpath, O_RDONLY, &v2, "real-probe-sbx3");
	CHECK(st == NTFSMAC_DMN_OK && v2.state == NTFSMAC_PROBE_STATE_CLEAN,
	      "%s: the probe sees the flag cleared (state %s)", what, ntfsmac_probe_state_name(v2.state));
}

static void case_real(void)
{
	char cmd[400], line[200], dev[64] = "";
	unsigned disk_no = 0;
	FILE *f;

	printf("== real: the real probe and volmark under their profiles (I-SBX-07)\n");
	if (g_probe_bin == NULL || g_volmark_bin == NULL || g_image == NULL) {
		FAIL("real: the case needs PROBE VOLMARK IMAGE");
		return;
	}
	real_on("image file", g_image, g_image);

	snprintf(cmd, sizeof(cmd),
		 "/usr/bin/hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage '%s' 2>&1", g_image);
	fflush(stdout);
	f = popen(cmd, "r");
	while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
		unsigned n;

		if (dev[0] == '\0' && sscanf(line, "/dev/disk%u", &n) == 1) {
			disk_no = n;
			snprintf(dev, sizeof(dev), "/dev/rdisk%u", n);
		}
	}
	if (f != NULL)
		pclose(f);
	if (dev[0] == '\0') {
		FAIL("real: could not attach the image as a raw disk");
		return;
	}
	printf("  raw disk: %s\n", dev);
	real_on("raw disk", dev, dev);
	snprintf(cmd, sizeof(cmd), "/usr/bin/hdiutil detach /dev/disk%u -force >/dev/null 2>&1", disk_no);
	fflush(stdout);
	if (system(cmd) != 0)
		printf("  NOTE: detach of %s failed\n", dev);
}

int main(int argc, char **argv)
{
	const char *only;

	if (argc < 6) {
		fprintf(stderr, "usage: sandbox_vm SANDBOX_DIR STUB UID GID WORKDIR [CASE]\n");
		return 2;
	}
	g_sbx_dir = argv[1];
	g_stub = argv[2];
	g_uid = (uid_t)strtoul(argv[3], NULL, 10);
	g_gid = (gid_t)strtoul(argv[4], NULL, 10);
	g_dir = argv[5];
	only = argc > 6 ? argv[6] : NULL;
	if (argc > 9) {
		g_probe_bin = argv[7];
		g_volmark_bin = argv[8];
		g_image = argv[9];
	} else if (argc > 7) {
		g_fuse_bin = argv[7];
	}
	if (geteuid() != 0 || g_uid == 0) {
		fprintf(stderr, "sandbox_vm: run as root, with a console user that is not root\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	atexit(cleanup);
	if (setup_guest() != 0)
		return 2;

	if (only == NULL || !strcmp(only, "probe"))
		case_probe_or_volmark(false);
	if (only == NULL || !strcmp(only, "volmark"))
		case_probe_or_volmark(true);
	if (only == NULL || !strcmp(only, "worker"))
		case_worker(false);
	if (only == NULL || !strcmp(only, "worker-um"))
		case_worker(true);
	if (only == NULL || !strcmp(only, "desc"))
		case_desc();
	if (only == NULL || !strcmp(only, "desc-broad"))
		case_desc_broad();
	if (only == NULL || !strcmp(only, "tree"))
		case_tree();
	if (only == NULL || !strcmp(only, "broad"))
		case_broad();
	if (only == NULL || !strcmp(only, "fail"))
		case_fail();
	if (only != NULL && !strcmp(only, "fuse2"))
		case_fuse2();
	if (only != NULL && !strcmp(only, "real"))
		case_real();
	printf("== result: %d failure(s)\n", g_fails);
	return g_fails == 0 ? 0 : 1;
}
