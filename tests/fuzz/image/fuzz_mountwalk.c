/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * F-MOUNT-WALK (docs/TEST_STRATEGY.md section 8): libFuzzer on libntfs-3g (with the release
 * patch series) mounting an in-memory image read-only, walking the tree and reading what the
 * FUSE glue of ntfs-3g reads: directory listings, path lookups, the unnamed data stream and
 * every named stream, the system xattrs (attributes, times, security descriptor, extended
 * attributes, reparse data, object id, DOS name, EFS info), symlink targets, owner and mode.
 * Compression, sparse runs and index blocks are exercised by the data reads and listings.
 *
 * The device is a struct ntfs_device_operations over the image bytes (image.h): reads are
 * served from memory, every write, and any request for a size or geometry, fails. The mount
 * flags are NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC; NTFS_MNT_RECOVER and NTFS_MNT_IGNORE_HIBERFILE
 * are never set. Three more choices come from the first input byte: system files shown,
 * case sensitive lookup, dot files hidden.
 *
 * Environment NTFSMAC_FUZZ_SKIP_SECURITY=1 leaves out the security descriptor reads (the ACL
 * xattr and the owner and mode). They hit two known libntfs-3g faults on corrupt images (a
 * heap read overflow in ntfs_valid_descr() and an allocation of up to 4 GiB in
 * retrievesecurityattr(), tests/fuzz/TRIAGE.md F1 and F3); a campaign that has recorded them
 * sets it so that they do not end every libFuzzer run.
 *
 * Oracles: no crash, no sanitizer report, no leak, and the library never asks the device to
 * write on a read-only mount. A walk is bounded (inodes, entries per directory, bytes read),
 * so a loop or a huge claimed size in a corrupt image costs coverage, not the run.
 */
#include "posix.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/logging.h>
#include <ntfs-3g/reparse.h>
#include <ntfs-3g/security.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/xattrs.h>

#include "image.h"

#define FAIL(msg)                                                                              \
	do {                                                                                   \
		fprintf(stderr, "F-MOUNT-WALK oracle failure: %s (line %d)\n", msg, __LINE__); \
		abort();                                                                       \
	} while (0)

#define MAX_INODES 400u
#define MAX_DIR_ENTRIES 200u
#define MAX_QUEUE 128u
#define MAX_PATH_BYTES 480u
#define MAX_STREAMS 6u
#define READ_CHUNK 4096u
#define READ_PER_STREAM (128u * 1024u)
#define READ_TOTAL (1024u * 1024u)
#define XATTR_BUF 65536u

/* ---- in-memory device ---------------------------------------------------------------------- */

typedef struct memdev {
	const uint8_t *buf;
	size_t size;
	s64 pos;
	unsigned long refused;
} memdev;

static memdev *priv(struct ntfs_device *dev)
{
	return (memdev *)dev->d_private;
}

static int mem_open(struct ntfs_device *dev, int flags)
{
	(void)flags; /* a read-write request is still served read-only */
	if (NDevOpen(dev)) {
		errno = EBUSY;
		return -1;
	}
	NDevSetReadOnly(dev);
	NDevSetOpen(dev);
	return 0;
}

static int mem_close(struct ntfs_device *dev)
{
	NDevClearOpen(dev);
	return 0;
}

static s64 mem_seek(struct ntfs_device *dev, s64 offset, int whence)
{
	memdev *m = priv(dev);
	s64 base;

	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = m->pos;
		break;
	case SEEK_END:
		base = (s64)m->size;
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	if ((offset > 0 && base > INT64_MAX - offset) || base + offset < 0) {
		errno = EINVAL;
		return -1;
	}
	m->pos = base + offset;
	return m->pos;
}

static s64 mem_pread(struct ntfs_device *dev, void *buf, s64 count, s64 offset)
{
	memdev *m = priv(dev);
	size_t n;

	if (count < 0 || offset < 0) {
		errno = EINVAL;
		return -1;
	}
	if ((uint64_t)offset >= m->size)
		return 0;
	n = m->size - (size_t)offset;
	if ((uint64_t)count < n)
		n = (size_t)count;
	memcpy(buf, m->buf + offset, n);
	return (s64)n;
}

static s64 mem_read(struct ntfs_device *dev, void *buf, s64 count)
{
	memdev *m = priv(dev);
	s64 r = mem_pread(dev, buf, count, m->pos);

	if (r > 0)
		m->pos += r;
	return r;
}

static s64 mem_write(struct ntfs_device *dev, const void *buf, s64 count)
{
	(void)buf;
	(void)count;
	priv(dev)->refused++;
	errno = EROFS;
	return -1;
}

static s64 mem_pwrite(struct ntfs_device *dev, const void *buf, s64 count, s64 offset)
{
	(void)buf;
	(void)count;
	(void)offset;
	priv(dev)->refused++;
	errno = EROFS;
	return -1;
}

static int mem_sync(struct ntfs_device *dev)
{
	(void)dev;
	return 0;
}

static int mem_stat(struct ntfs_device *dev, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFREG | 0444;
	st->st_size = (off_t)priv(dev)->size;
	st->st_blksize = 4096;
	st->st_nlink = 1;
	return 0;
}

static int mem_ioctl(struct ntfs_device *dev, unsigned long request, void *argp)
{
	(void)dev;
	(void)request;
	(void)argp;
	errno = ENOTTY;
	return -1;
}

static struct ntfs_device_operations mem_ops = {
	.open = mem_open,
	.close = mem_close,
	.seek = mem_seek,
	.read = mem_read,
	.write = mem_write,
	.pread = mem_pread,
	.pwrite = mem_pwrite,
	.sync = mem_sync,
	.stat = mem_stat,
	.ioctl = mem_ioctl,
};

/* ---- the walk -------------------------------------------------------------------------------- */

typedef struct entry {
	MFT_REF mref;
	unsigned dt_type;
	ntfschar *name;
	int name_len;
} entry;

typedef struct dirlist {
	entry e[MAX_DIR_ENTRIES];
	unsigned n;
	bool full;
} dirlist;

typedef struct qitem {
	u64 mft_no;
	char path[MAX_PATH_BYTES + 1];
} qitem;

typedef struct walk {
	ntfs_volume *vol;
	struct SECURITY_CONTEXT scx;
	struct PERMISSIONS_CACHE *seccache;
	u64 seen[MAX_INODES];
	unsigned nseen;
	qitem queue[MAX_QUEUE];
	unsigned qhead, qtail;
	size_t read_total;
	char *buf;
} walk;

static int fill(void *dirent, const ntfschar *name, const int name_len, const int name_type, const s64 pos,
		const MFT_REF mref, const unsigned dt_type)
{
	dirlist *d = dirent;
	entry *e;

	(void)pos;
	if (name_type == FILE_NAME_DOS)
		return 0;
	if (d->n >= MAX_DIR_ENTRIES) {
		d->full = true;
		return -1;
	}
	if (name_len < 0 || name_len > 255)
		return -1;
	e = &d->e[d->n];
	e->name = malloc((size_t)(name_len ? name_len : 1) * sizeof(ntfschar));
	if (e->name == NULL)
		return -1;
	memcpy(e->name, name, (size_t)name_len * sizeof(ntfschar));
	e->name_len = name_len;
	e->mref = mref;
	e->dt_type = dt_type;
	d->n++;
	return 0;
}

static void free_dirlist(dirlist *d)
{
	unsigned i;

	for (i = 0; i < d->n; i++)
		free(d->e[i].name);
	d->n = 0;
}

static bool mark_seen(walk *w, u64 mft_no)
{
	unsigned i;

	for (i = 0; i < w->nseen; i++)
		if (w->seen[i] == mft_no)
			return false;
	if (w->nseen >= MAX_INODES)
		return false;
	w->seen[w->nseen++] = mft_no;
	return true;
}

/* Read the stream the way the FUSE read path does: sequential chunks, then the middle and the end. */
static void read_stream(walk *w, ntfs_inode *ni, const ntfschar *name, u32 name_len)
{
	ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, (ntfschar *)name, name_len);
	s64 size, off, got, limit;

	if (na == NULL)
		return;
	size = na->data_size;
	limit = size < (s64)READ_PER_STREAM ? size : (s64)READ_PER_STREAM;
	for (off = 0; off < limit && w->read_total < READ_TOTAL; off += READ_CHUNK) {
		got = ntfs_attr_pread(na, off, READ_CHUNK, w->buf);
		if (got <= 0)
			break;
		w->read_total += (size_t)got;
	}
	if (size > 2 * (s64)READ_CHUNK && w->read_total < READ_TOTAL) {
		got = ntfs_attr_pread(na, size / 2, READ_CHUNK, w->buf);
		if (got > 0)
			w->read_total += (size_t)got;
		got = ntfs_attr_pread(na, size - READ_CHUNK / 2, READ_CHUNK, w->buf);
		if (got > 0)
			w->read_total += (size_t)got;
	}
	ntfs_attr_close(na);
}

/* Names of the named $DATA streams, as ntfs_fuse_listxattr() finds them. */
static void read_streams(walk *w, ntfs_inode *ni)
{
	ntfs_attr_search_ctx *actx = ntfs_attr_get_search_ctx(ni, NULL);
	ntfschar names[MAX_STREAMS][256];
	u32 lens[MAX_STREAMS];
	unsigned n = 0, i;

	if (actx == NULL)
		return;
	while (n < MAX_STREAMS && !ntfs_attr_lookup(AT_DATA, NULL, 0, CASE_SENSITIVE, 0, NULL, 0, actx)) {
		ATTR_RECORD *a = actx->attr;
		char *mbs = NULL;

		if (a->name_length == 0)
			continue;
		memcpy(names[n], (u8 *)a + le16_to_cpu(a->name_offset), (size_t)a->name_length * sizeof(ntfschar));
		lens[n] = a->name_length;
		if (ntfs_ucstombs(names[n], (int)lens[n], &mbs, 0) >= 0)
			free(mbs);
		n++;
	}
	ntfs_attr_put_search_ctx(actx);
	for (i = 0; i < n; i++)
		read_stream(w, ni, names[i], lens[i]);
}

static bool skip_security;

static void get_xattrs(walk *w, ntfs_inode *ni, ntfs_inode *dir_ni)
{
	static const enum SYSTEMXATTRS types[] = {
		XATTR_NTFS_ACL,       XATTR_NTFS_ATTRIB,      XATTR_NTFS_ATTRIB_BE, XATTR_NTFS_EFSINFO,
		XATTR_NTFS_REPARSE_DATA, XATTR_NTFS_OBJECT_ID, XATTR_NTFS_DOS_NAME,  XATTR_NTFS_TIMES,
		XATTR_NTFS_TIMES_BE,  XATTR_NTFS_CRTIME,      XATTR_NTFS_CRTIME_BE, XATTR_NTFS_EA,
	};
	size_t i;
	struct stat st;

	for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		if (skip_security && types[i] == XATTR_NTFS_ACL)
			continue;
		(void)ntfs_xattr_system_getxattr(&w->scx, types[i], ni, dir_ni, w->buf, XATTR_BUF);
	}
	/* a too small buffer is a different path (size query) */
	if (!skip_security)
		(void)ntfs_xattr_system_getxattr(&w->scx, XATTR_NTFS_ACL, ni, dir_ni, w->buf, 16);
	(void)ntfs_xattr_system_getxattr(&w->scx, XATTR_NTFS_EA, ni, dir_ni, w->buf, 4);
	memset(&st, 0, sizeof(st));
	if (!skip_security)
		(void)ntfs_get_owner_mode(&w->scx, ni, &st);
}

static void visit(walk *w, ntfs_inode *dir_ni, const char *dirpath, const entry *e)
{
	ntfs_inode *ni;
	char *mbs = NULL, path[MAX_PATH_BYTES + 1];
	bool have_path = false;
	u64 mft_no = MREF(e->mref);

	if (e->name_len > 0 && ntfs_ucstombs(e->name, e->name_len, &mbs, 0) >= 0 && mbs != NULL) {
		size_t dl = strlen(dirpath), nl = strlen(mbs);

		if (dl + 1 + nl <= MAX_PATH_BYTES && strchr(mbs, '/') == NULL) {
			memcpy(path, dirpath, dl);
			path[dl] = '/';
			memcpy(path + dl + 1, mbs, nl + 1);
			have_path = true;
		}
	}
	free(mbs);
	if (!mark_seen(w, mft_no))
		return;
	ni = ntfs_inode_open(w->vol, MK_MREF(mft_no, 0));
	if (ni == NULL)
		return;

	if (have_path) {
		ntfs_inode *by_path = ntfs_pathname_to_inode(w->vol, NULL, path);

		if (by_path != NULL)
			(void)ntfs_inode_close(by_path);
	}
	get_xattrs(w, ni, dir_ni);
	if (ni->mrec != NULL && (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
		if (have_path && w->qtail < MAX_QUEUE) {
			w->queue[w->qtail].mft_no = mft_no;
			memcpy(w->queue[w->qtail].path, path, strlen(path) + 1);
			w->qtail++;
		}
	} else {
		char *target;

		read_stream(w, ni, AT_UNNAMED, 0);
		read_streams(w, ni);
		if (ni->flags & FILE_ATTR_REPARSE_POINT) {
			target = ntfs_make_symlink(ni, "/mnt");
			free(target);
		}
		(void)ntfs_interix_types(ni);
	}
	(void)ntfs_inode_close(ni);
}

static void walk_dir(walk *w, u64 mft_no, const char *path)
{
	ntfs_inode *dir_ni = ntfs_inode_open(w->vol, MK_MREF(mft_no, 0));
	dirlist *d;
	s64 pos = 0;
	unsigned i;

	if (dir_ni == NULL)
		return;
	d = calloc(1, sizeof(*d));
	if (d == NULL)
		abort();
	(void)ntfs_readdir(dir_ni, &pos, d, fill);
	read_streams(w, dir_ni); /* named streams on a directory */
	for (i = 0; i < d->n; i++)
		visit(w, dir_ni, path, &d->e[i]);
	free_dirlist(d);
	free(d);
	(void)ntfs_inode_close(dir_ni);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static bool init;
	static walk *w;
	const uint8_t *img;
	size_t isize;
	memdev m;
	struct ntfs_device *dev;
	ntfs_volume *vol;
	unsigned choice = size > 0 ? (data[0] / 3u) & 7u : 0;

	if (!init) {
		ntfs_log_set_handler(ntfs_log_handler_null);
		(void)ntfs_set_char_encoding("UTF-8");
		skip_security = getenv("NTFSMAC_FUZZ_SKIP_SECURITY") != NULL;
		w = malloc(sizeof(*w));
		if (w == NULL)
			abort();
		w->buf = malloc(XATTR_BUF);
		if (w->buf == NULL)
			abort();
		init = true;
	}
	img = fz_image_build(data, size, &isize);
	memset(&m, 0, sizeof(m));
	m.buf = img;
	m.size = isize;

	dev = ntfs_device_alloc("ntfsmac-fuzz-image", 0, &mem_ops, &m);
	if (dev == NULL)
		FAIL("ntfs_device_alloc");
	vol = ntfs_device_mount(dev, NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC);
	if (vol == NULL) {
		/* a failed ntfs_device_mount() closes the device but does not free it */
		ntfs_device_free(dev);
		if (m.refused != 0)
			FAIL("the library wrote to the device while failing a read-only mount");
		return 0;
	}
	if (choice & 1u)
		NVolSetShowSysFiles(vol);
	if (choice & 2u)
		NVolSetCaseSensitive(vol);
	if (choice & 4u)
		NVolSetHideDotFiles(vol);

	memset(&w->scx, 0, sizeof(w->scx));
	w->seccache = NULL;
	w->scx.vol = vol;
	w->scx.pseccache = &w->seccache;
	vol->secure_flags = 1u << SECURITY_WANTED;
	(void)ntfs_open_secure(vol);
	(void)ntfs_build_mapping(&w->scx, NULL, TRUE);

	w->vol = vol;
	w->nseen = 0;
	w->qhead = w->qtail = 0;
	w->read_total = 0;
	w->queue[w->qtail].mft_no = FILE_root;
	w->queue[w->qtail].path[0] = '\0';
	w->qtail++;
	mark_seen(w, FILE_root);
	while (w->qhead < w->qtail && w->nseen < MAX_INODES && w->read_total < READ_TOTAL) {
		qitem it = w->queue[w->qhead++];

		walk_dir(w, it.mft_no, it.path);
	}

	ntfs_destroy_security_context(&w->scx);
	(void)ntfs_close_secure(vol);
	(void)ntfs_umount(vol, FALSE);
	if (m.refused != 0)
		FAIL("the library wrote to the device on a read-only mount");
	return 0;
}
