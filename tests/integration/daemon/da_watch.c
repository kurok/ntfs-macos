/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Experiment tool for T-M4-02 (docs/spikes/M4-da.md). Runs the Disk
 * Arbitration module (src/daemon/da.c) against real diskarbitrationd and
 * prints one line per event. Run it as root inside a disposable macOS guest;
 * never on the host. It never writes to a device: the open tests open a
 * descriptor and close it again.
 *
 * Build (guest):
 *   cc -std=c17 -Wall -Wextra -I src/daemon -I include -o da_watch \
 *      tests/integration/daemon/da_watch.c src/daemon/da.c src/daemon/da_filter.c src/daemon/status.c \
 *      -framework DiskArbitration -framework CoreFoundation
 *
 * Usage: da_watch [--size BYTES] [--dissent] [--unmount] [--open-test]
 *                 [--handback S] [--duration S]
 *   --size BYTES   act only on disks of exactly this size (the test disk);
 *                  other disks are only reported
 *   --dissent      dissent from inside the disk_appeared callback
 *   --unmount      unmount the disk through DA when its mount is reported
 *   --open-test    open the device O_RDONLY and O_RDWR when a mount is reported
 *                  and after an unmount, and print the errno of each
 *   --handback S   withdraw the dissent S seconds after the disk appeared
 *   --duration S   run time, default 60
 * Output goes to stdout, one line per event, "T+seconds event ...".
 */
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define QMAX 64

typedef enum { ACT_MOUNTED, ACT_UNMOUNTED, ACT_APPEARED } act_kind;

typedef struct {
	act_kind kind;
	char bsd[32];
} act;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static act g_q[QMAX];
static unsigned g_qn;
static struct timespec g_t0;
static uint64_t g_size;
static bool g_dissent, g_unmount, g_open_test;
static int g_handback = -1;
static ntfsmac_da *g_da;

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)(t.tv_sec - g_t0.tv_sec) + (double)(t.tv_nsec - g_t0.tv_nsec) / 1e9;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	va_list ap;

	pthread_mutex_lock(&g_lock);
	printf("T+%.3f ", now());
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	pthread_mutex_unlock(&g_lock);
}

static void push(act_kind k, const char *bsd)
{
	pthread_mutex_lock(&g_lock);
	if (g_qn < QMAX) {
		g_q[g_qn].kind = k;
		snprintf(g_q[g_qn].bsd, sizeof g_q[g_qn].bsd, "%s", bsd);
		g_qn++;
	}
	pthread_mutex_unlock(&g_lock);
}

static bool target(const ntfsmac_disk_info *d)
{
	return g_size == 0 || d->size_bytes == g_size;
}

static void print_info(const char *ev, const ntfsmac_disk_info *d)
{
	say("%s %s size=%llu whole=%d cand=%d volume_path=%d kind=%s media_path=%s", ev,
	    d->bsd_name, (unsigned long long)d->size_bytes, d->whole_disk, d->is_ntfs_candidate,
	    d->has_volume_path, d->volume_kind[0] != '\0' ? d->volume_kind : "-", d->media_path);
}

static void cb_appeared(const ntfsmac_disk_info *d, void *ctx)
{
	(void)ctx;
	print_info("appeared", d);
	if (g_dissent && target(d)) {
		ntfsmac_dmn_status st = ntfsmac_da_dissent(g_da, d->bsd_name);

		say("dissent %s -> %s", d->bsd_name, ntfsmac_dmn_status_name(st));
	}
	if (target(d))
		push(ACT_APPEARED, d->bsd_name);
}

static void cb_disappeared(const char *bsd, void *ctx)
{
	(void)ctx;
	say("disappeared %s", bsd);
}

static void cb_mounted(const ntfsmac_disk_info *d, void *ctx)
{
	(void)ctx;
	print_info("mounted", d);
	if (target(d))
		push(ACT_MOUNTED, d->bsd_name);
}

static void cb_unmounted(const char *bsd, void *ctx)
{
	(void)ctx;
	say("unmounted %s", bsd);
	push(ACT_UNMOUNTED, bsd);
}

static void open_test(const char *bsd, const char *tag)
{
	static const char *const prefix[] = { "/dev/", "/dev/r" };
	unsigned i;

	for (i = 0; i < 2; i++) {
		char path[64];
		int fd, e1, e2;

		snprintf(path, sizeof path, "%s%s", prefix[i], bsd);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		e1 = fd < 0 ? errno : 0;
		if (fd >= 0)
			close(fd);
		fd = open(path, O_RDWR | O_CLOEXEC);
		e2 = fd < 0 ? errno : 0;
		if (fd >= 0)
			close(fd);
		say("open-test %s %s O_RDONLY=%s O_RDWR=%s", tag, path,
		    e1 == 0 ? "ok" : strerror(e1), e2 == 0 ? "ok" : strerror(e2));
	}
}

int main(int argc, char **argv)
{
	ntfsmac_da_callbacks cb = { cb_appeared, cb_disappeared, cb_mounted, cb_unmounted };
	int duration = 60, i;
	double first_appeared = -1;
	bool handed = false;
	char last_target[32] = "";
	ntfsmac_dmn_status st;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--size") == 0 && i + 1 < argc)
			g_size = strtoull(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--dissent") == 0)
			g_dissent = true;
		else if (strcmp(argv[i], "--unmount") == 0)
			g_unmount = true;
		else if (strcmp(argv[i], "--open-test") == 0)
			g_open_test = true;
		else if (strcmp(argv[i], "--handback") == 0 && i + 1 < argc)
			g_handback = atoi(argv[++i]);
		else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
			duration = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: da_watch [--size B] [--dissent] [--unmount] "
					"[--open-test] [--handback S] [--duration S]\n");
			return 2;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &g_t0);
	st = ntfsmac_da_start(&cb, NULL, &g_da);
	say("start -> %s uid=%d", ntfsmac_dmn_status_name(st), (int)getuid());
	if (st != NTFSMAC_DMN_OK)
		return 1;
	while (now() < duration) {
		act a;
		bool have = false;

		pthread_mutex_lock(&g_lock);
		if (g_qn > 0) {
			a = g_q[0];
			memmove(&g_q[0], &g_q[1], (g_qn - 1) * sizeof g_q[0]);
			g_qn--;
			have = true;
		}
		pthread_mutex_unlock(&g_lock);
		if (!have) {
			if (g_handback >= 0 && last_target[0] != '\0' && !handed &&
			    now() >= first_appeared + g_handback) {
				handed = true;
				st = ntfsmac_da_withdraw(g_da, last_target);
				say("withdraw %s -> %s", last_target, ntfsmac_dmn_status_name(st));
			}
			usleep(50000);
			continue;
		}
		if (a.kind == ACT_APPEARED && first_appeared < 0) {
			first_appeared = now();
			snprintf(last_target, sizeof last_target, "%s", a.bsd);
		}
		if (a.kind == ACT_MOUNTED) {
			ntfsmac_disk_info d;

			st = ntfsmac_da_describe(g_da, a.bsd, &d);
			say("describe %s -> %s volume_path=%d kind=%s", a.bsd,
			    ntfsmac_dmn_status_name(st), d.has_volume_path,
			    d.volume_kind[0] != '\0' ? d.volume_kind : "-");
			if (g_open_test)
				open_test(a.bsd, "while-mounted");
			if (g_unmount) {
				double t = now();

				st = ntfsmac_da_unmount(g_da, a.bsd);
				say("unmount %s -> %s (%.3f s)", a.bsd, ntfsmac_dmn_status_name(st),
				    now() - t);
				if (g_open_test)
					open_test(a.bsd, "after-unmount");
			}
		}
	}
	say("stop");
	ntfsmac_da_stop(g_da);
	return 0;
}
