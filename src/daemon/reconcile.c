/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Restart reconciliation (T-M4-05; docs/ARCHITECTURE.md section 7, row
 * "ntfsmacd crash", and section 3.5).
 *
 * When the daemon starts it looks at the session journal and at the live
 * mounts. For each journal entry it decides one of:
 *
 *   KEEP_ENTRY      no writable live mount belongs to the entry. The entry
 *                   stays. The next mount of that volume is read-only.
 *   READOPT         one writable mount, served by a live worker, and the
 *                   entry says the dirty marker was set (or the marker step
 *                   is off). The daemon takes the mount over. The read end of
 *                   the session channel died with the old daemon, so this
 *                   session can never be proven clean: the entry and the
 *                   marker stay when it ends.
 *   FORCE_UNMOUNT   a writable mount exists but it cannot be re-adopted
 *                   (no live worker, an inconsistent entry, or the mount
 *                   cannot be told apart from another one). The daemon
 *                   unmounts it with the forced path. The entry stays.
 *
 * This module never removes or changes a journal entry. Only the four paths
 * of section 3.5 remove entries, and none of them is a restart.
 *
 * How a mount is tied to an entry (T-M7-12): the daemon created the mount
 * point /Volumes/<leaf> from the sanitised label (section 4.1 step 7), and the
 * entry holds the same sanitised label, so the leaf must be the label, or the
 * label followed by " 2" to " 99". A label is chosen by whoever made the disk,
 * so the name alone proves nothing: once the worker's mount appeared, the
 * daemon recorded in the entry what the kernel says about it in struct statfs
 * (owner, f_fsid, f_mntfromname; no path walk, root never looks inside the
 * served file system, section 4.1 step 8), and a live mount belongs to the
 * entry only when all three are the same now. An entry without that record
 * (format 1, or a daemon that stopped before writing it) matches no mount: its
 * mount is left alone and logged. Two entries that claim one mount remain
 * ambiguous; ambiguity is resolved towards safety: such a writable mount is
 * force-unmounted, never re-adopted. The entry stays in every case, so the
 * volume is read-only on the next mount.
 *
 * The decision code is pure. Only ntfsmac_reconcile_list_mounts() calls the
 * system, and only on macOS; ntfsmac_reconcile_run_with() logs.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "ntfsmac/log.h"
#include "sanitize.h"

#define VOLUMES_PREFIX "/Volumes/"
#define VOLUMES_PREFIX_LEN (sizeof(VOLUMES_PREFIX) - 1u)

/* ------------------------------------------------------------------ */
/* Pure decision logic                                                 */
/* ------------------------------------------------------------------ */

/* The label the mount-point leaf was built from (section 4.1 step 7). */
static size_t entry_leaf_base(const ntfsmac_journal_entry *e, char *out, size_t cap)
{
	size_t n = strnlen(e->label, sizeof e->label);

	if (n == 0 || n >= cap) {
		/* An empty label gets the fallback leaf of ADR-016. */
		return ntfsmac_label_sanitize("", 0, e->serial, out, cap);
	}
	memcpy(out, e->label, n);
	out[n] = '\0';
	return n;
}

/* "2" to "99", no leading zero. */
static bool suffix_2_to_99(const char *s)
{
	if (s[0] < '0' || s[0] > '9')
		return false;
	if (s[1] == '\0')
		return s[0] >= '2';
	if (s[1] < '0' || s[1] > '9' || s[2] != '\0')
		return false;
	return s[0] != '0';
}

bool ntfsmac_reconcile_name_matches(const ntfsmac_journal_entry *e, const char *mount_point)
{
	char base[NTFSMAC_LABEL_PART_SIZE];
	size_t base_len;
	const char *leaf;

	if (e == NULL || mount_point == NULL)
		return false;
	if (strncmp(mount_point, VOLUMES_PREFIX, VOLUMES_PREFIX_LEN) != 0)
		return false;
	leaf = mount_point + VOLUMES_PREFIX_LEN;
	if (strchr(leaf, '/') != NULL)
		return false;
	base_len = entry_leaf_base(e, base, sizeof base);
	if (base_len == 0)
		return false;
	if (strncmp(leaf, base, base_len) != 0)
		return false;
	if (leaf[base_len] == '\0')
		return true;
	return leaf[base_len] == ' ' && suffix_2_to_99(leaf + base_len + 1);
}

bool ntfsmac_reconcile_mount_matches(const ntfsmac_journal_entry *e, const ntfsmac_live_mount *m)
{
	if (e == NULL || m == NULL || !ntfsmac_reconcile_name_matches(e, m->mount_point))
		return false;
	/* The name is steered by a disk label; only what the kernel says about the mount counts. */
	return e->has_mount && !e->legacy && m->owner == e->mount_owner && m->fsid == e->mount_fsid &&
	       m->from[0] != '\0' && strcmp(m->from, e->mount_from) == 0;
}

bool ntfsmac_reconcile_left_alone(const ntfsmac_journal_entry *entries, size_t n_entries,
				  const ntfsmac_live_mount *m)
{
	size_t i;
	bool named = false;

	if (m == NULL || m->read_only || (n_entries != 0 && entries == NULL))
		return false;
	for (i = 0; i < n_entries; i++) {
		if (ntfsmac_reconcile_mount_matches(&entries[i], m))
			return false; /* the session's own mount: the plan acts on it */
		if (ntfsmac_reconcile_name_matches(&entries[i], m->mount_point))
			named = true;
	}
	return named;
}

size_t ntfsmac_reconcile_count_left(const ntfsmac_journal_entry *entries, size_t n_entries,
				    const ntfsmac_live_mount *mounts, size_t n_mounts)
{
	size_t i, n = 0;

	if (mounts == NULL)
		return 0;
	for (i = 0; i < n_mounts; i++)
		if (ntfsmac_reconcile_left_alone(entries, n_entries, &mounts[i]))
			n++;
	return n;
}

static pthread_mutex_t g_left_mu = PTHREAD_MUTEX_INITIALIZER;
static size_t g_left_mounted;

void ntfsmac_reconcile_set_left_mounted(size_t n)
{
	pthread_mutex_lock(&g_left_mu);
	g_left_mounted = n;
	pthread_mutex_unlock(&g_left_mu);
}

size_t ntfsmac_reconcile_left_mounted(void)
{
	size_t n;

	pthread_mutex_lock(&g_left_mu);
	n = g_left_mounted;
	pthread_mutex_unlock(&g_left_mu);
	return n;
}

void ntfsmac_mount_from_copy(char *dst, size_t cap, const char *src, size_t srcsize)
{
	size_t n, i;

	if (dst == NULL || cap == 0)
		return;
	dst[0] = '\0';
	if (src == NULL)
		return;
	n = strnlen(src, srcsize);
	if (n == 0 || n >= cap)
		return;
	for (i = 0; i < n; i++)
		if ((unsigned char)src[i] < 0x20 || (unsigned char)src[i] > 0x7e)
			return;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

/*
 * Entry facts that allow re-adoption of a writable session. `pending`: the
 * worker starts only after `volmark set`, so a live writable mount with
 * `pending` is inconsistent. `failed`: that session was to be read-only.
 */
static bool marker_allows_readopt(const ntfsmac_journal_entry *e)
{
	return e->marker == NTFSMAC_JOURNAL_MARKER_SET ||
	       e->marker == NTFSMAC_JOURNAL_MARKER_NONE;
}

static void put_item(ntfsmac_reconcile_item *it, size_t entry, ntfsmac_reconcile_action a,
		     long mount)
{
	it->entry_index = entry;
	it->action = a;
	it->mount_index = mount;
}

size_t ntfsmac_reconcile_plan(const ntfsmac_journal_entry *entries, size_t n_entries,
			      const ntfsmac_live_mount *mounts, size_t n_mounts,
			      ntfsmac_reconcile_item *items, size_t cap)
{
	size_t out = 0;
	size_t i, m;
	unsigned char *emitted;
	unsigned *claims;

	if (n_entries == 0 || entries == NULL || items == NULL)
		return 0;
	if (n_mounts != 0 && mounts == NULL)
		return 0;
	if (n_entries + n_mounts > cap)
		return 0;

	emitted = calloc(n_mounts != 0 ? n_mounts : 1, 1);
	claims = calloc(n_mounts != 0 ? n_mounts : 1, sizeof *claims);
	if (emitted == NULL || claims == NULL) {
		free(emitted);
		free(claims);
		return 0;
	}

	/* How many entries claim each mount. */
	for (i = 0; i < n_entries; i++)
		for (m = 0; m < n_mounts; m++)
			if (ntfsmac_reconcile_mount_matches(&entries[i], &mounts[m]))
				claims[m]++;

	for (i = 0; i < n_entries; i++) {
		size_t candidates = 0, writable = 0, first_writable = 0;
		size_t before = out;

		for (m = 0; m < n_mounts; m++) {
			if (!ntfsmac_reconcile_mount_matches(&entries[i], &mounts[m]))
				continue;
			candidates++;
			if (!mounts[m].read_only) {
				if (writable == 0)
					first_writable = m;
				writable++;
			}
		}

		if (writable == 0) {
			/* No mount, or only read-only mounts: nothing to adopt. */
			put_item(&items[out++], i, NTFSMAC_RECONCILE_KEEP_ENTRY, -1);
			continue;
		}

		if (candidates == 1 && claims[first_writable] == 1 &&
		    mounts[first_writable].worker_pid > 0 && marker_allows_readopt(&entries[i])) {
			put_item(&items[out++], i, NTFSMAC_RECONCILE_READOPT, (long)first_writable);
			emitted[first_writable] = 1;
			continue;
		}

		for (m = 0; m < n_mounts; m++) {
			if (mounts[m].read_only || emitted[m] ||
			    !ntfsmac_reconcile_mount_matches(&entries[i], &mounts[m]))
				continue;
			put_item(&items[out++], i, NTFSMAC_RECONCILE_FORCE_UNMOUNT, (long)m);
			emitted[m] = 1;
		}
		if (out == before) {
			/* Every writable mount was already taken by another entry. */
			put_item(&items[out++], i, NTFSMAC_RECONCILE_KEEP_ENTRY, -1);
		}
	}

	free(emitted);
	free(claims);
	return out;
}

/* ------------------------------------------------------------------ */
/* Run                                                                 */
/* ------------------------------------------------------------------ */

typedef struct collect {
	ntfsmac_journal_entry *v;
	size_t n;
	size_t cap;
	bool oom;
} collect;

static int collect_entry(const ntfsmac_journal_entry *e, void *ctx)
{
	collect *c = ctx;

	if (c->n == c->cap) {
		size_t ncap = c->cap != 0 ? c->cap * 2 : 8;
		ntfsmac_journal_entry *nv = realloc(c->v, ncap * sizeof *nv);

		if (nv == NULL) {
			c->oom = true;
			return 1;
		}
		c->v = nv;
		c->cap = ncap;
	}
	c->v[c->n++] = *e;
	return 0;
}

ntfsmac_dmn_status ntfsmac_reconcile_run_with(ntfsmac_journal *j,
					      const ntfsmac_live_mount *mounts,
					      size_t n_mounts, ntfsmac_reconcile_cb cb,
					      void *ctx)
{
	return ntfsmac_reconcile_run_limit(j, mounts, n_mounts, 0, cb, ctx, NULL);
}

ntfsmac_dmn_status ntfsmac_reconcile_run_limit(ntfsmac_journal *j,
					       const ntfsmac_live_mount *mounts,
					       size_t n_mounts, size_t max_items,
					       ntfsmac_reconcile_cb cb, void *ctx,
					       size_t *unlisted)
{
	collect c = {NULL, 0, 0, false};
	ntfsmac_reconcile_item *items;
	size_t n_items, i;
	ntfsmac_journal_status js;
	ntfsmac_journal_list_info info;

	if (unlisted != NULL)
		*unlisted = 0;
	if (j == NULL || cb == NULL || (n_mounts != 0 && mounts == NULL))
		return NTFSMAC_DMN_E_INVAL;

	js = ntfsmac_journal_list_ex(j, max_items, collect_entry, &c, &info);
	if (c.oom || js != NTFSMAC_JOURNAL_OK) {
		free(c.v);
		return NTFSMAC_DMN_E_IO;
	}
	if (info.unlisted != 0) {
		/* Reconcile what was read; the rest keep their entries and are decided by their identity. */
		NTFSMAC_LOGW("restart reconciliation: the journal has %zu entries, %zu were not listed "
			     "(over the bound of one listing); reconciling the %zu that were. "
			     "`ntfsmac doctor` names `journal-clear`",
			     info.found, info.unlisted, c.n);
		if (unlisted != NULL)
			*unlisted = info.unlisted;
	}
	if (c.n == 0) {
		free(c.v);
		ntfsmac_reconcile_set_left_mounted(0);
		return NTFSMAC_DMN_OK;
	}

	items = calloc(c.n + n_mounts, sizeof *items);
	if (items == NULL) {
		free(c.v);
		return NTFSMAC_DMN_E_IO;
	}
	/*
	 * A writable mount that has an entry's name and is no entry's session mount: say why it
	 * stays, and keep the count for `doctor`, `status` and the notification (owner decision
	 * 2026-11-26: it is left mounted, with a warning).
	 */
	for (i = 0; i < n_mounts; i++)
		if (ntfsmac_reconcile_left_alone(c.v, c.n, &mounts[i]))
			NTFSMAC_LOGW("reconcile: %s has the name of a journal entry but is not that "
				     "session's mount (owner, id or source differ, or none was "
				     "recorded): left mounted; the entry stays",
				     mounts[i].mount_point);
	ntfsmac_reconcile_set_left_mounted(ntfsmac_reconcile_count_left(c.v, c.n, mounts, n_mounts));
	n_items = ntfsmac_reconcile_plan(c.v, c.n, mounts, n_mounts, items, c.n + n_mounts);
	/*
	 * A cut listing does not show every entry, so a mount that looks claimed by one entry only may
	 * be claimed by an unlisted one too (the "only one claim" rule of the plan cannot be checked).
	 * No mount is adopted then: each READOPT becomes FORCE_UNMOUNT, the fail-closed choice.
	 */
	for (i = 0; info.unlisted != 0 && i < n_items; i++)
		if (items[i].action == NTFSMAC_RECONCILE_READOPT)
			items[i].action = NTFSMAC_RECONCILE_FORCE_UNMOUNT;
	for (i = 0; i < n_items; i++) {
		const ntfsmac_reconcile_item *it = &items[i];
		const ntfsmac_live_mount *m = it->mount_index >= 0 ? &mounts[it->mount_index] : NULL;

		if (cb(&c.v[it->entry_index], it->action, m, ctx) != 0)
			break;
	}
	free(items);
	free(c.v);
	return n_items != 0 ? NTFSMAC_DMN_OK : NTFSMAC_DMN_E_IO;
}

ntfsmac_dmn_status ntfsmac_reconcile_run(ntfsmac_journal *j, ntfsmac_reconcile_cb cb,
					 void *ctx)
{
	ntfsmac_live_mount *mounts = NULL;
	size_t n = 0;
	ntfsmac_dmn_status st;

	if (j == NULL || cb == NULL)
		return NTFSMAC_DMN_E_INVAL;
	st = ntfsmac_reconcile_list_mounts(&mounts, &n);
	if (st != NTFSMAC_DMN_OK)
		return st;
	st = ntfsmac_reconcile_run_with(j, mounts, n, cb, ctx);
	free(mounts);
	return st;
}

/* ------------------------------------------------------------------ */
/* Platform: list the live mounts                                      */
/* ------------------------------------------------------------------ */

#if defined(__APPLE__)

#include <sys/mount.h>
#include <sys/param.h>

/*
 * File system type names of the macFUSE mounts. [U] The exact name that the
 * FSKit backend reports in f_fstypename is not recorded in the project
 * documents; T-M4-08 and T-M4-11 confirm it in the VM. The list is short
 * and in one place on purpose. A mount with another type name is never
 * touched, so an APFS volume that has the same name as an NTFS label cannot
 * be unmounted by mistake.
 */
static bool fstype_is_ours(const char *name)
{
	static const char *const prefixes[] = {"macfuse", "osxfuse", "fusefs"};
	size_t i;

	for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
		if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0)
			return true;
	return false;
}

ntfsmac_dmn_status ntfsmac_reconcile_list_mounts(ntfsmac_live_mount **out, size_t *n)
{
	struct statfs *fs;
	ntfsmac_live_mount *v;
	int count, got, i;
	size_t used = 0;

	if (out == NULL || n == NULL)
		return NTFSMAC_DMN_E_INVAL;
	*out = NULL;
	*n = 0;

	count = getfsstat(NULL, 0, MNT_NOWAIT);
	if (count < 0)
		return NTFSMAC_DMN_E_IO;
	if (count == 0)
		return NTFSMAC_DMN_OK;
	fs = calloc((size_t)count, sizeof *fs);
	v = calloc((size_t)count, sizeof *v);
	if (fs == NULL || v == NULL) {
		free(fs);
		free(v);
		return NTFSMAC_DMN_E_IO;
	}
	got = getfsstat(fs, (int)((size_t)count * sizeof *fs), MNT_NOWAIT);
	if (got < 0) {
		free(fs);
		free(v);
		return NTFSMAC_DMN_E_IO;
	}
	for (i = 0; i < got && i < count; i++) {
		size_t len = strnlen(fs[i].f_mntonname, sizeof fs[i].f_mntonname);

		if (!fstype_is_ours(fs[i].f_fstypename))
			continue;
		if (len >= sizeof v[used].mount_point)
			continue;
		if (strncmp(fs[i].f_mntonname, VOLUMES_PREFIX, VOLUMES_PREFIX_LEN) != 0)
			continue;
		memcpy(v[used].mount_point, fs[i].f_mntonname, len);
		v[used].mount_point[len] = '\0';
		v[used].read_only = (fs[i].f_flags & MNT_RDONLY) != 0;
		v[used].owner = (uint32_t)fs[i].f_owner;
		memcpy(&v[used].fsid, &fs[i].f_fsid, sizeof v[used].fsid);
		ntfsmac_mount_from_copy(v[used].from, sizeof v[used].from, fs[i].f_mntfromname,
					sizeof fs[i].f_mntfromname);
		/*
		 * No worker is looked up. Finding the process that serves a mount
		 * needs the process table and the worker's arguments, and a wrong
		 * guess would re-adopt a session we cannot vouch for. With
		 * worker_pid 0 every writable mount is force-unmounted, which is
		 * the fallback that section 7 allows. A later task may fill this.
		 */
		v[used].worker_pid = 0;
		used++;
	}
	free(fs);
	if (used == 0) {
		free(v);
		return NTFSMAC_DMN_OK;
	}
	*out = v;
	*n = used;
	return NTFSMAC_DMN_OK;
}

#else /* !__APPLE__ */

ntfsmac_dmn_status ntfsmac_reconcile_list_mounts(ntfsmac_live_mount **out, size_t *n)
{
	if (out != NULL)
		*out = NULL;
	if (n != NULL)
		*n = 0;
	return NTFSMAC_DMN_E_NOTIMPL;
}

#endif
