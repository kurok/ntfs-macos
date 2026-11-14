/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * fd_pass - I-SMOKE-02 helper (T-M2-05): a root parent opens a device (or an image file),
 * then runs a program as an unprivileged user with only that descriptor inherited, named
 * to the program as /dev/fd/N. This models ARCHITECTURE.md section 4.1 steps 6 and 8:
 * the daemon opens the device, the worker runs as the console user without permission
 * on the device node and gets the descriptor by inheritance.
 *
 * Runs IN THE macOS GUEST only, as root (sudo -n), on a device or image the caller made.
 *
 * Usage:
 *   fd_pass check TARGET EXPECT_BYTES
 *       Open TARGET read-only, check its size (block count * block size for a device,
 *       st_size for a file) equals EXPECT_BYTES and that sector 0 is an NTFS boot sector
 *       ("NTFS    " at offset 3, 0x55AA at 510). Exit 0 if both hold.
 *   fd_pass semantics TARGET USER
 *       Record what open("/dev/fd/N") does on this system: shared offset, shared status
 *       flags, access mode of the new descriptor, whether a wider mode than the original
 *       is refused, and whether an unprivileged process can do the same on an inherited
 *       descriptor. Prints "key=value" lines. Never writes to TARGET.
 *   fd_pass run ro|rw|wo TARGET EXPECT_BYTES USER PROG [ARG...]
 *       check TARGET as above, open it O_RDONLY (ro), O_RDWR (rw) or O_WRONLY (wo, negative
 *       tests only), fork, drop to USER
 *       (setgroups, setgid, setuid; verified irreversible), verify USER cannot open
 *       TARGET by path, then exec PROG with only fds 0-2 and the device descriptor
 *       (posix_spawn with POSIX_SPAWN_SETEXEC | POSIX_SPAWN_CLOEXEC_DEFAULT). Every ARG
 *       equal to "@FD@" is replaced by /dev/fd/N, and "@UID@"/"@GID@" inside an ARG by
 *       USER's ids. Exit status is PROG's (128 + signal if killed; 70 on a setup error).
 *
 * Exit: 0 ok, 1 check failed, 2 usage error, 70 internal/setup error.
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/disk.h>
#endif

extern char **environ;

#define EX_SETUP 70

static void usage(void)
{
	fprintf(stderr,
		"usage: fd_pass check TARGET EXPECT_BYTES\n"
		"       fd_pass semantics TARGET USER\n"
		"       fd_pass run ro|rw|wo TARGET EXPECT_BYTES USER PROG [ARG...]\n");
	exit(2);
}

static const char *mode_name(int fl)
{
	switch (fl & O_ACCMODE) {
	case O_RDONLY: return "O_RDONLY";
	case O_WRONLY: return "O_WRONLY";
	case O_RDWR:   return "O_RDWR";
	default:       return "?";
	}
}

static int target_size(int fd, uint64_t *out)
{
	struct stat st;

	if (fstat(fd, &st) != 0)
		return -1;
	if (S_ISREG(st.st_mode)) {
		*out = (uint64_t)st.st_size;
		return 0;
	}
#if defined(__APPLE__)
	if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
		uint64_t count = 0;
		uint32_t bsize = 0;

		if (ioctl(fd, DKIOCGETBLOCKCOUNT, &count) != 0
		    || ioctl(fd, DKIOCGETBLOCKSIZE, &bsize) != 0)
			return -1;
		*out = count * bsize;
		return 0;
	}
#endif
	errno = ENOTSUP;
	return -1;
}

/* Size equals expect and sector 0 looks like an NTFS boot sector. */
static int check_fd(int fd, const char *target, uint64_t expect)
{
	unsigned char bs[512];
	uint64_t size = 0;
	struct stat st;

	if (fstat(fd, &st) != 0 || target_size(fd, &size) != 0) {
		fprintf(stderr, "fd_pass: %s: size: %s\n", target, strerror(errno));
		return 1;
	}
	printf("check: %s type=%s size=%llu expect=%llu\n", target,
	       S_ISBLK(st.st_mode) ? "block" : S_ISCHR(st.st_mode) ? "char" : "file",
	       (unsigned long long)size, (unsigned long long)expect);
	if (size != expect) {
		fprintf(stderr, "fd_pass: %s: size %llu != expected %llu, refusing\n", target,
			(unsigned long long)size, (unsigned long long)expect);
		return 1;
	}
	if (pread(fd, bs, sizeof(bs), 0) != (ssize_t)sizeof(bs)) {
		fprintf(stderr, "fd_pass: %s: read sector 0: %s\n", target, strerror(errno));
		return 1;
	}
	if (memcmp(bs + 3, "NTFS    ", 8) != 0 || bs[510] != 0x55 || bs[511] != 0xAA) {
		fprintf(stderr, "fd_pass: %s: no NTFS boot sector, refusing\n", target);
		return 1;
	}
	printf("check: %s has an NTFS boot sector\n", target);
	return 0;
}

static int parse_u64(const char *s, uint64_t *out)
{
	char *end;
	unsigned long long v;

	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || end == s || *end || s[0] == '-')
		return -1;
	*out = v;
	return 0;
}

static int lookup_user(const char *name, uid_t *uid, gid_t *gid)
{
	struct passwd *pw = getpwnam(name);

	if (!pw) {
		fprintf(stderr, "fd_pass: unknown user %s\n", name);
		return -1;
	}
	if (pw->pw_uid == 0) {
		fprintf(stderr, "fd_pass: user %s is root, refusing\n", name);
		return -1;
	}
	*uid = pw->pw_uid;
	*gid = pw->pw_gid;
	return 0;
}

/* Drop to uid/gid with no supplementary groups; verify root cannot be regained. */
static int drop_privs(uid_t uid, gid_t gid)
{
	if (setgroups(1, &gid) != 0 || setgid(gid) != 0 || setuid(uid) != 0) {
		fprintf(stderr, "fd_pass: drop privileges: %s\n", strerror(errno));
		return -1;
	}
	if (setuid(0) == 0 || seteuid(0) == 0 || getuid() != uid || geteuid() != uid
	    || getgid() != gid || getegid() != gid) {
		fprintf(stderr, "fd_pass: privileges not dropped\n");
		return -1;
	}
	return 0;
}

static int cmd_check(int argc, char **argv)
{
	uint64_t expect;
	int fd, rc;

	if (argc != 4 || parse_u64(argv[3], &expect) != 0)
		usage();
	fd = open(argv[2], O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "fd_pass: open %s: %s\n", argv[2], strerror(errno));
		return 1;
	}
	rc = check_fd(fd, argv[2], expect);
	close(fd);
	return rc;
}

/* Probe /dev/fd/N behaviour for an open descriptor fd; prefix tags the output lines. */
static void probe_devfd(const char *prefix, int fd)
{
	char path[32];
	int fl_orig, fl_new, nfd, wfd;
	off_t a, b;
	struct stat s1, s2;

	snprintf(path, sizeof(path), "/dev/fd/%d", fd);
	fl_orig = fcntl(fd, F_GETFL);
	printf("%s.orig_mode=%s\n", prefix, mode_name(fl_orig));

	if (lseek(fd, 4096, SEEK_SET) != 4096) {
		printf("%s.lseek_orig=error:%s\n", prefix, strerror(errno));
		return;
	}
	nfd = open(path, O_RDONLY);
	if (nfd < 0) {
		printf("%s.open_rdonly=error:%s\n", prefix, strerror(errno));
		return;
	}
	printf("%s.open_rdonly=ok\n", prefix);
	fl_new = fcntl(nfd, F_GETFL);
	printf("%s.new_mode_after_O_RDONLY_open=%s\n", prefix, mode_name(fl_new));

	/* Offset sharing: the original was seeked to 4096 before the open. */
	a = lseek(nfd, 0, SEEK_CUR);
	printf("%s.new_offset_at_open=%lld (orig at 4096)\n", prefix, (long long)a);
	(void)lseek(nfd, 8192, SEEK_SET);
	b = lseek(fd, 0, SEEK_CUR);
	printf("%s.orig_offset_after_new_seek_8192=%lld\n", prefix, (long long)b);
	printf("%s.shares_offset=%s\n", prefix, (a == 4096 && b == 8192) ? "yes" : "no");

	/* Status flag sharing: set O_NONBLOCK on the new one, read it on the original. */
	(void)fcntl(nfd, F_SETFL, fl_new | O_NONBLOCK);
	printf("%s.shares_status_flags=%s\n", prefix,
	       (fcntl(fd, F_GETFL) & O_NONBLOCK) ? "yes" : "no");
	(void)fcntl(nfd, F_SETFL, fl_new & ~O_NONBLOCK);
	(void)fcntl(fd, F_SETFL, fl_orig & ~O_NONBLOCK);

	if (fstat(fd, &s1) == 0 && fstat(nfd, &s2) == 0)
		printf("%s.same_dev_ino_rdev=%s\n", prefix,
		       (s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino
			&& s1.st_rdev == s2.st_rdev) ? "yes" : "no");
	printf("%s.fd_cloexec_on_new=%s\n", prefix,
	       (fcntl(nfd, F_GETFD) & FD_CLOEXEC) ? "yes" : "no");
	close(nfd);

	/* A wider access mode than the original. Opened only, never written. */
	wfd = open(path, O_RDWR);
	if (wfd < 0) {
		printf("%s.open_rdwr=error:%s\n", prefix, strerror(errno));
	} else {
		printf("%s.open_rdwr=ok new_mode=%s\n", prefix, mode_name(fcntl(wfd, F_GETFL)));
		close(wfd);
	}
	/* O_RDWR|O_EXCL is what libntfs-3g unix_io.c uses for a non-block rw open. */
	wfd = open(path, O_RDWR | O_EXCL);
	if (wfd < 0) {
		printf("%s.open_rdwr_excl=error:%s\n", prefix, strerror(errno));
	} else {
		printf("%s.open_rdwr_excl=ok new_mode=%s\n", prefix,
		       mode_name(fcntl(wfd, F_GETFL)));
		close(wfd);
	}
	/* The advisory lock libntfs-3g takes on the device. */
	{
		struct flock fl;

		memset(&fl, 0, sizeof(fl));
		fl.l_type = ((fl_orig & O_ACCMODE) == O_RDWR) ? F_WRLCK : F_RDLCK;
		fl.l_whence = SEEK_SET;
		printf("%s.fcntl_setlk_%s=%s\n", prefix,
		       fl.l_type == F_WRLCK ? "wr" : "rd",
		       fcntl(fd, F_SETLK, &fl) == 0 ? "ok" : strerror(errno));
		fl.l_type = F_UNLCK;
		(void)fcntl(fd, F_SETLK, &fl);
	}
	(void)lseek(fd, 0, SEEK_SET);
}

static int cmd_semantics(int argc, char **argv)
{
	static const int modes[] = { O_RDONLY, O_RDWR };
	uid_t uid;
	gid_t gid;
	unsigned i;

	if (argc != 4)
		usage();
	if (geteuid() != 0) {
		fprintf(stderr, "fd_pass: semantics must run as root\n");
		return 2;
	}
	if (lookup_user(argv[3], &uid, &gid) != 0)
		return 2;
	printf("target=%s\n", argv[2]);
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		char prefix[32];
		int fd, st;
		pid_t pid;

		fd = open(argv[2], modes[i]);
		if (fd < 0) {
			fprintf(stderr, "fd_pass: open %s: %s\n", argv[2], strerror(errno));
			return 1;
		}
		snprintf(prefix, sizeof(prefix), "root.%s", mode_name(modes[i]));
		probe_devfd(prefix, fd);
		fflush(stdout);

		/* Same probe in an unprivileged child that only inherited the descriptor. */
		pid = fork();
		if (pid < 0)
			return EX_SETUP;
		if (pid == 0) {
			int pfd;

			if (drop_privs(uid, gid) != 0)
				_exit(EX_SETUP);
			snprintf(prefix, sizeof(prefix), "user.%s", mode_name(modes[i]));
			pfd = open(argv[2], O_RDONLY);
			printf("%s.open_by_path=%s\n", prefix, pfd < 0 ? strerror(errno) : "ok");
			if (pfd >= 0)
				close(pfd);
			probe_devfd(prefix, fd);
			fflush(stdout);
			_exit(0);
		}
		if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
			fprintf(stderr, "fd_pass: semantics child failed\n");
			close(fd);
			return 1;
		}
		close(fd);
	}
	return 0;
}

/* Replace every occurrence of key in s by val; returns a malloc'ed string. */
static char *subst(const char *s, const char *key, const char *val)
{
	size_t kl = strlen(key), vl = strlen(val), n = 0;
	const char *p;
	char *out, *o;

	for (p = s; (p = strstr(p, key)) != NULL; p += kl)
		n++;
	out = malloc(strlen(s) + n * vl + 1);
	if (!out)
		return NULL;
	o = out;
	while ((p = strstr(s, key)) != NULL) {
		memcpy(o, s, (size_t)(p - s));
		o += p - s;
		memcpy(o, val, vl);
		o += vl;
		s = p + kl;
	}
	strcpy(o, s);
	return out;
}

static int cmd_run(int argc, char **argv)
{
	const char *mode, *target, *user;
	uint64_t expect;
	uid_t uid;
	gid_t gid;
	int fd, st, i, flags;
	pid_t pid;

	if (argc < 7 || parse_u64(argv[4], &expect) != 0)
		usage();
	mode = argv[2];
	target = argv[3];
	user = argv[5];
	if (strcmp(mode, "ro") == 0)
		flags = O_RDONLY;
	else if (strcmp(mode, "rw") == 0)
		flags = O_RDWR;
	else if (strcmp(mode, "wo") == 0)
		flags = O_WRONLY; /* negative test only: a write-only descriptor */
	else
		usage();
	if (geteuid() != 0) {
		fprintf(stderr, "fd_pass: run must run as root\n");
		return 2;
	}
	if (lookup_user(user, &uid, &gid) != 0)
		return 2;

	/* Check through a read-only descriptor first (a write-only one cannot read). */
	fd = open(target, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "fd_pass: open %s: %s\n", target, strerror(errno));
		return 1;
	}
	st = check_fd(fd, target, expect);
	close(fd);
	if (st != 0)
		return 1;
	/* O_CLOEXEC here; the spawn below marks only this descriptor inheritable. */
	fd = open(target, flags | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "fd_pass: open %s %s: %s\n", target, mode_name(flags), strerror(errno));
		return 1;
	}
	printf("parent: uid=%d opened %s %s as fd %d\n", (int)getuid(), target,
	       mode_name(fcntl(fd, F_GETFL)), fd);
	fflush(stdout);

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fd_pass: fork: %s\n", strerror(errno));
		return EX_SETUP;
	}
	if (pid == 0) {
		char fdpath[32], uids[16], gids[16];
		char **cargv;
		posix_spawnattr_t attr;
		posix_spawn_file_actions_t fa;
		int pfd, err, n = argc - 6;

		if (drop_privs(uid, gid) != 0)
			_exit(EX_SETUP);
		/* The child must not be able to open the target itself. */
		pfd = open(target, O_RDONLY);
		if (pfd >= 0) {
			fprintf(stderr, "fd_pass: child uid %d CAN open %s by path; test invalid\n",
				(int)uid, target);
			_exit(EX_SETUP);
		}
		printf("child: uid=%d gid=%d open(%s) by path: %s (expected)\n", (int)getuid(),
		       (int)getgid(), target, strerror(errno));

		snprintf(fdpath, sizeof(fdpath), "/dev/fd/%d", fd);
		snprintf(uids, sizeof(uids), "%d", (int)uid);
		snprintf(gids, sizeof(gids), "%d", (int)gid);
		cargv = calloc((size_t)n + 1, sizeof(*cargv));
		if (!cargv)
			_exit(EX_SETUP);
		for (i = 0; i < n; i++) {
			const char *a = argv[6 + i];
			char *t;

			if (strcmp(a, "@FD@") == 0) {
				cargv[i] = fdpath;
				continue;
			}
			t = subst(a, "@UID@", uids);
			cargv[i] = t ? subst(t, "@GID@", gids) : NULL;
			free(t);
			if (!cargv[i])
				_exit(EX_SETUP);
		}
		printf("child: exec");
		for (i = 0; i < n; i++)
			printf(" %s", cargv[i]);
		printf("\n");
		fflush(stdout);

		/* Clear FD_CLOEXEC on the one descriptor that must survive the exec. */
		if (fcntl(fd, F_SETFD, 0) != 0
		    || posix_spawnattr_init(&attr) != 0 || posix_spawn_file_actions_init(&fa) != 0)
			_exit(EX_SETUP);
		err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
						| POSIX_SPAWN_CLOEXEC_DEFAULT
#endif
		);
		if (!err)
			err = posix_spawn_file_actions_addinherit_np(&fa, 0);
		if (!err)
			err = posix_spawn_file_actions_addinherit_np(&fa, 1);
		if (!err)
			err = posix_spawn_file_actions_addinherit_np(&fa, 2);
		if (!err)
			err = posix_spawn_file_actions_addinherit_np(&fa, fd);
		if (!err)
			err = posix_spawnp(NULL, cargv[0], &fa, &attr, cargv, environ);
		fprintf(stderr, "fd_pass: spawn %s: %s\n", cargv[0], strerror(err));
		_exit(EX_SETUP);
	}
	close(fd);
	if (waitpid(pid, &st, 0) != pid) {
		fprintf(stderr, "fd_pass: waitpid: %s\n", strerror(errno));
		return EX_SETUP;
	}
	if (WIFSIGNALED(st)) {
		printf("parent: child killed by signal %d\n", WTERMSIG(st));
		return 128 + WTERMSIG(st);
	}
	printf("parent: child exit %d\n", WEXITSTATUS(st));
	return WEXITSTATUS(st);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2)
		usage();
	if (strcmp(argv[1], "check") == 0)
		return cmd_check(argc, argv);
	if (strcmp(argv[1], "semantics") == 0)
		return cmd_semantics(argc, argv);
	if (strcmp(argv[1], "run") == 0)
		return cmd_run(argc, argv);
	usage();
	return 2;
}
