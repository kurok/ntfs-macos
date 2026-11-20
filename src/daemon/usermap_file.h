/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reading and re-checking the usermapping file (docs/ARCHITECTURE.md section 3.3, ADR-012). The
 * file is opened by the daemon without following a link, checked on the descriptor, read and
 * parsed; the descriptor stays open until just before the spawn, where the path is compared with
 * it once more. The decisions are in usermap.h.
 */
#ifndef NTFSMAC_DAEMON_USERMAP_FILE_H
#define NTFSMAC_DAEMON_USERMAP_FILE_H

#include "usermap.h"

typedef struct ntfsmac_usermap_file {
	int fd; /* -1 unless the file was accepted */
	ntfsmac_usermap_ident opened;
	ntfsmac_usermap_parse parse;
} ntfsmac_usermap_file;

void ntfsmac_usermap_file_init(ntfsmac_usermap_file *f);
void ntfsmac_usermap_file_close(ntfsmac_usermap_file *f);

/*
 * Opens `path` (O_RDONLY | O_NOFOLLOW | O_CLOEXEC, and O_NONBLOCK so that a FIFO cannot block),
 * checks the descriptor with ntfsmac_usermap_stat_verdict() (regular, owned by `owner`, mode
 * 0644, at most 1 MiB), reads exactly its size, parses, and decides with
 * ntfsmac_usermap_decide() (descriptor unchanged, path still the same file). Returns ENABLE only
 * then, with `f->fd` open; otherwise `f->fd` is -1. `*bad_line` is the parser's line number (0
 * if none) and `*parse_out`, if not NULL, the parser's result, for the log.
 */
ntfsmac_usermap_verdict ntfsmac_usermap_file_open(const char *path, uid_t owner,
						  ntfsmac_usermap_file *f, unsigned *bad_line,
						  ntfsmac_usermap_parse *parse_out);

/* The same decision again, with the descriptor kept open since the read: the spawn calls it last. */
ntfsmac_usermap_verdict ntfsmac_usermap_file_reverify(const char *path, uid_t owner,
						      const ntfsmac_usermap_file *f);

#endif /* NTFSMAC_DAEMON_USERMAP_FILE_H */
