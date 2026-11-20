/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The usermapping file of the opt-in `usermapping=` (ADR-012, docs/ARCHITECTURE.md section 3.3):
 * the grammar of the engine, and the decision whether the option may be enabled for a file. Pure
 * code, no I/O, so the host tests can drive it. The I/O (open, fstat, read, re-check) is in
 * usermap_file.c.
 *
 * The grammar is a strict subset of what NTFS-3G 2026.9.28 accepts (libntfs-3g/acls.c:
 * ntfs_read_mapping, getmappingitem, encodesid, ntfs_do_user_mapping, ntfs_do_group_mapping,
 * ntfs_valid_pattern). The engine silently drops or ignores much of what it does not like (a bad
 * line ends the reading, a last line without newline is ignored, a login name that does not
 * resolve gives uid 0 and the line is dropped, a root mapping is dropped, ...), so the mapping it
 * applies would not be the one the administrator wrote. This parser accepts a file only when
 * the engine, as far as its source shows, applies every mapping line of it the way it is written
 * and applies the mapping at all (it needs a user and a group mapping, and it never looks past
 * the implicit entry). Where the engine reads more than the grammar below, the file is refused.
 */
#ifndef NTFSMAC_DAEMON_USERMAP_H
#define NTFSMAC_DAEMON_USERMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#define NTFSMAC_USERMAP_MAX_SIZE (1024u * 1024u) /* bytes; larger files are refused */
#define NTFSMAC_USERMAP_LINE_MAX 120u            /* LINESZ of the engine: longer lines are cut by it */
#define NTFSMAC_USERMAP_MODE 0644                /* st_mode & 07777, exactly */

typedef enum ntfsmac_usermap_parse {
	NTFSMAC_USERMAP_P_OK = 0,
	NTFSMAC_USERMAP_P_INVAL,     /* bad argument */
	NTFSMAC_USERMAP_P_TOO_BIG,   /* more than NTFSMAC_USERMAP_MAX_SIZE bytes */
	NTFSMAC_USERMAP_P_EMPTY,     /* no mapping line (only comments, or nothing) */
	NTFSMAC_USERMAP_P_NO_EOL,    /* the last line has no newline: the engine ignores it */
	NTFSMAC_USERMAP_P_BLANK,     /* empty line: the engine stops reading there */
	NTFSMAC_USERMAP_P_CHAR,      /* control character, NUL, CR, or a non-ASCII byte in a mapping line */
	NTFSMAC_USERMAP_P_LINE_LONG, /* mapping line over NTFSMAC_USERMAP_LINE_MAX bytes */
	NTFSMAC_USERMAP_P_SYNTAX,    /* not exactly uid:gid:SID */
	NTFSMAC_USERMAP_P_UID,       /* not empty and not a number from 1 to 2147483647 */
	NTFSMAC_USERMAP_P_GID,       /* same, for the group */
	NTFSMAC_USERMAP_P_SID,       /* not S-1-<authority>-<1 to 8 sub-authorities>, numbers up to 4294967295 */
	NTFSMAC_USERMAP_P_SID_KIND,  /* a SID the engine does not take for this kind of line */
	NTFSMAC_USERMAP_P_PATTERN,   /* implicit ::SID whose last number is not 1000 to 2147483647 (ADR-012) */
	NTFSMAC_USERMAP_P_INCOMPLETE, /* no user mapping or no group mapping: the engine applies nothing */
	NTFSMAC_USERMAP_P_AFTER_IMPLICIT, /* a mapping line after the implicit one: never reached */
	NTFSMAC_USERMAP_P_DUPLICATE   /* a uid, group-only gid or SID that an earlier line has */
} ntfsmac_usermap_parse;

/*
 * Checks `len` bytes at `buf` (the file contents; not a C string). On a refusal `*bad_line` is the
 * 1-based number of the offending line (0 when no single line is meant: too big, empty). Grammar:
 *
 *   file    = line+, every line ends with "\n"; blank lines only after the last mapping line
 *   line    = comment | mapping
 *   comment = "#" any bytes but control characters (tab is allowed), at the start of the line
 *   mapping = uid ":" gid ":" SID, at most 120 bytes, printable ASCII
 *   uid,gid = empty | decimal number 1..2147483647 (no login names: the engine would look them up
 *             in the name service, and an unknown name drops the line silently)
 *   SID     = "S-1-" authority ("-" sub-authority){1,8}, decimal numbers up to 4294967295
 *
 * and the engine's kinds: "::SID" (implicit pattern) needs a user SID (S-1-5-21-a-b-c-rid, five
 * sub-authorities) whose last number is 1000 to 2147483647; a line with a uid needs a user SID;
 * a line with only a gid takes a user SID or a well-known group SID (S-1-5-n, n 1 to 6).
 * More refusals, each with the line number: a blank line followed by any later mapping line (the
 * engine ends the reading at the first blank line; blank and comment lines up to the end of the
 * file lose nothing); a mapping line after the implicit one, or a second implicit one (the
 * engine's lookups stop at the implicit entry); a repeated uid, a repeated gid of a group-only
 * line, or a repeated SID. A file needs at least one user mapping (a uid, or the implicit line) and
 * one group mapping (a gid, or the implicit line): ntfs_build_mapping() applies nothing otherwise
 * (line 0).
 */
ntfsmac_usermap_parse ntfsmac_usermap_parse_text(const char *buf, size_t len, unsigned *bad_line);

const char *ntfsmac_usermap_parse_str(ntfsmac_usermap_parse p);

/* What the daemon knows about a file: the result of fstat(fd) or lstat(path). */
typedef struct ntfsmac_usermap_ident {
	dev_t dev;
	ino_t ino;
	mode_t mode; /* st_mode, with the file type */
	uid_t uid;
	off_t size;
	int64_t mtime_s;
	int64_t mtime_ns;
	int64_t ctime_s;
	int64_t ctime_ns;
} ntfsmac_usermap_ident;

void ntfsmac_usermap_ident_from_stat(ntfsmac_usermap_ident *id, const struct stat *st);

typedef enum ntfsmac_usermap_verdict {
	NTFSMAC_USERMAP_V_ENABLE = 0, /* the option may be enabled for this file */
	NTFSMAC_USERMAP_V_NOT_REGULAR,
	NTFSMAC_USERMAP_V_NOT_OWNER,  /* not owned by the required uid (root) */
	NTFSMAC_USERMAP_V_BAD_MODE,   /* not exactly 0644 */
	NTFSMAC_USERMAP_V_TOO_BIG,
	NTFSMAC_USERMAP_V_UNREADABLE, /* open or read failed, or a short or long read (I/O layer) */
	NTFSMAC_USERMAP_V_PARSE,      /* the parser refused the content */
	NTFSMAC_USERMAP_V_CHANGED,    /* the file or the name changed between the check and now */
	NTFSMAC_USERMAP_V_GONE        /* the name no longer leads to a file (or is a link) */
} ntfsmac_usermap_verdict;

/* The checks of a descriptor's fstat alone (type, owner, mode, size), before the file is read. */
ntfsmac_usermap_verdict ntfsmac_usermap_stat_verdict(const ntfsmac_usermap_ident *opened, uid_t owner);

/*
 * Whether `usermapping=` may be enabled. `opened` is the fstat of the descriptor that was read,
 * `parse` the parser's result for the bytes read from it, `fd_now` the fstat of the same
 * descriptor now, and `path_now` the lstat (no follow) of the path now, NULL if that failed.
 * Order of the checks: the descriptor's own state, the parse, the descriptor unchanged since it
 * was read, and the path still naming that very file. Anything else than ENABLE means the mount
 * goes ahead without the option. Called once after the read and once right before the spawn.
 */
ntfsmac_usermap_verdict ntfsmac_usermap_decide(const ntfsmac_usermap_ident *opened, uid_t owner,
					       ntfsmac_usermap_parse parse,
					       const ntfsmac_usermap_ident *fd_now,
					       const ntfsmac_usermap_ident *path_now);

const char *ntfsmac_usermap_verdict_str(ntfsmac_usermap_verdict v);

#endif /* NTFSMAC_DAEMON_USERMAP_H */
