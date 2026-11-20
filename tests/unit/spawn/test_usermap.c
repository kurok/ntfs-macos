/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-UMP-01..06: the grammar of the usermapping file and the decision whether `usermapping=` may be
 * enabled (src/daemon/usermap.c, issue 2: ADR-012 and audit M4 F-03). Pure code: strings and
 * fake stat results, no file, no device, no mount.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "usermap.h"

#define DOM "S-1-5-21-1111111111-2222222222-3333333333-"

typedef struct pcase {
	const char *what;
	const char *text;
	ntfsmac_usermap_parse want;
	unsigned line;
} pcase;

static void run_cases(const pcase *c, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned line = 99;
		ntfsmac_usermap_parse r = ntfsmac_usermap_parse_text(c[i].text, strlen(c[i].text), &line);

		if (r != c[i].want || line != c[i].line)
			fprintf(stderr, "case \"%s\": got %d line %u, want %d line %u\n", c[i].what, (int)r, line,
				(int)c[i].want, c[i].line);
		CHECK_EQ_INT(r, c[i].want);
		CHECK_EQ_INT(line, c[i].line);
	}
}

static ntfsmac_usermap_ident good_ident(void)
{
	ntfsmac_usermap_ident id;

	memset(&id, 0, sizeof(id));
	id.dev = 5;
	id.ino = 4242;
	id.mode = S_IFREG | 0644;
	id.uid = 0;
	id.size = 100;
	id.mtime_s = 1700000000;
	id.mtime_ns = 123;
	id.ctime_s = 1700000001;
	id.ctime_ns = 456;
	return id;
}

int main(void)
{
	/* U-UMP-01: what the engine reads as written is accepted. */
	static const pcase ok[] = {
		{ "implicit pattern", "::" DOM "1000\n", NTFSMAC_USERMAP_P_OK, 0 },
		{ "implicit pattern, top of the range", "::" DOM "2147483647\n", NTFSMAC_USERMAP_P_OK, 0 },
		{ "user line", "501:20:" DOM "1001\n", NTFSMAC_USERMAP_P_OK, 0 },
		{ "user line without gid, and a group line", "501::" DOM "1001\n:20:" DOM "513\n", NTFSMAC_USERMAP_P_OK, 0 },
		{ "same gid for two users", "501:20:" DOM "1001\n502:20:" DOM "1002\n", NTFSMAC_USERMAP_P_OK, 0 },
		{ "blank and comment lines at the end", "::" DOM "1000\n\n# end\n\n", NTFSMAC_USERMAP_P_OK, 0 },
		{ "group line", "501::" DOM "1001\n:20:" DOM "513\n", NTFSMAC_USERMAP_P_OK, 0 },
		{ "group line, well-known group SID", "501::" DOM "1001\n:20:S-1-5-6\n", NTFSMAC_USERMAP_P_OK, 0 },
		{ "comments and lines", "# the mapping\n501:20:" DOM "1001\n# more, with a\ttab\n::" DOM "1000\n",
		  NTFSMAC_USERMAP_P_OK, 0 },
		{ "UTF-8 in a comment", "# caf\xc3\xa9\n::" DOM "1000\n", NTFSMAC_USERMAP_P_OK, 0 },
	};
	/* U-UMP-02: ADR-012: an implicit pattern ending below 1000 is refused, with its line number. */
	static const pcase pattern[] = {
		{ "implicit 999", "::" DOM "999\n", NTFSMAC_USERMAP_P_PATTERN, 1 },
		{ "implicit 0", "::" DOM "0\n", NTFSMAC_USERMAP_P_PATTERN, 1 },
		{ "implicit 500 on line 3", "# c\n501:20:" DOM "1001\n::" DOM "500\n", NTFSMAC_USERMAP_P_PATTERN, 3 },
		{ "implicit 2147483648", "::" DOM "2147483648\n", NTFSMAC_USERMAP_P_PATTERN, 1 },
		{ "implicit 4294967295", "::" DOM "4294967295\n", NTFSMAC_USERMAP_P_PATTERN, 1 },
		{ "implicit with a well-known group SID", "::S-1-5-4\n", NTFSMAC_USERMAP_P_SID_KIND, 1 },
		{ "implicit with a short SID", "::S-1-5-21-1-2000\n", NTFSMAC_USERMAP_P_SID_KIND, 1 },
	};
	/* U-UMP-03: a line the grammar does not know is refused (fail closed). */
	static const pcase syntax[] = {
		{ "no colon", "hello\n", NTFSMAC_USERMAP_P_SYNTAX, 1 },
		{ "one colon", "501:" DOM "1001\n", NTFSMAC_USERMAP_P_SYNTAX, 1 },
		{ "fourth field (the engine ignores it)", "501:20:" DOM "1001:x\n", NTFSMAC_USERMAP_P_SYNTAX, 1 },
		{ "login name as user", "joe:20:" DOM "1001\n", NTFSMAC_USERMAP_P_UID, 1 },
		{ "login name as group", "501:staff:" DOM "1001\n", NTFSMAC_USERMAP_P_GID, 1 },
		{ "root uid (the engine drops it)", "0:20:" DOM "1001\n", NTFSMAC_USERMAP_P_UID, 1 },
		{ "root gid", ":0:" DOM "1001\n", NTFSMAC_USERMAP_P_GID, 1 },
		{ "uid with a trailing letter (atoi reads 12)", "12a:20:" DOM "1001\n", NTFSMAC_USERMAP_P_UID, 1 },
		{ "uid with a sign", "+5:20:" DOM "1001\n", NTFSMAC_USERMAP_P_UID, 1 },
		{ "uid over int", "2147483648:20:" DOM "1001\n", NTFSMAC_USERMAP_P_UID, 1 },
		{ "uid far over int", "99999999999999999999:20:" DOM "1001\n", NTFSMAC_USERMAP_P_UID, 1 },
		{ "space in a field", "501 :20:" DOM "1001\n", NTFSMAC_USERMAP_P_UID, 1 },
		{ "SID without S-1-", "501:20:S-2-5-21-1-2-3-1001\n", NTFSMAC_USERMAP_P_SID, 1 },
		{ "SID with junk after the digits (the engine reads 1001)", "501:20:" DOM "1001x\n",
		  NTFSMAC_USERMAP_P_SID, 1 },
		{ "SID with an empty component", "501:20:S-1-5-21--2-3-1001\n", NTFSMAC_USERMAP_P_SID, 1 },
		{ "SID with a trailing dash", "501:20:S-1-5-21-1-2-3-\n", NTFSMAC_USERMAP_P_SID, 1 },
		{ "SID with only an authority", "501:20:S-1-5\n", NTFSMAC_USERMAP_P_SID, 1 },
		{ "SID component over 32 bits", "501:20:S-1-5-21-1-2-3-4294967296\n", NTFSMAC_USERMAP_P_SID, 1 },
		{ "SID with nine sub-authorities", "501:20:S-1-5-1-2-3-4-5-6-7-8-9\n", NTFSMAC_USERMAP_P_SID, 1 },
		{ "empty SID", "501:20:\n", NTFSMAC_USERMAP_P_SID, 1 },
		{ "user line with a well-known group SID (the engine: Bad user SID)", "501:20:S-1-5-6\n",
		  NTFSMAC_USERMAP_P_SID_KIND, 1 },
		{ "group line with an unknown SID kind", ":20:S-1-5-32-544\n", NTFSMAC_USERMAP_P_SID_KIND, 1 },
		{ "second line is the bad one", "501:20:" DOM "1001\nbad\n", NTFSMAC_USERMAP_P_SYNTAX, 2 },
	};
	/* U-UMP-04: the structure of the file, where the engine would drop or ignore text silently. */
	static const pcase structure[] = {
		{ "empty file", "", NTFSMAC_USERMAP_P_EMPTY, 0 },
		{ "comments only", "# a\n# b\n", NTFSMAC_USERMAP_P_EMPTY, 0 },
		{ "last line without newline", "501:20:" DOM "1001", NTFSMAC_USERMAP_P_NO_EOL, 1 },
		{ "last comment without newline", "::" DOM "1000\n# end", NTFSMAC_USERMAP_P_NO_EOL, 2 },
		{ "blank line between lines (the engine stops there)", "501:20:" DOM "1001\n\n502:21:" DOM "1002\n",
		  NTFSMAC_USERMAP_P_BLANK, 2 },
		{ "blank line, then a mapping line", "\n::" DOM "1000\n", NTFSMAC_USERMAP_P_BLANK, 1 },
		{ "blank line, a comment, then a mapping line", "501:20:" DOM "1001\n\n# c\n502:21:" DOM "1002\n",
		  NTFSMAC_USERMAP_P_BLANK, 2 },
		{ "indented comment is a mapping line", " # x\n", NTFSMAC_USERMAP_P_SYNTAX, 1 },
		{ "CRLF", "::" DOM "1000\r\n", NTFSMAC_USERMAP_P_CHAR, 1 },
		{ "CR in a comment", "# a\r\n::" DOM "1000\n", NTFSMAC_USERMAP_P_CHAR, 1 },
		{ "tab in a mapping line", "501:\t20:" DOM "1001\n", NTFSMAC_USERMAP_P_CHAR, 1 },
		{ "non-ASCII in a mapping line", "501:20:" DOM "1001\xc3\xa9\n", NTFSMAC_USERMAP_P_CHAR, 1 },
		{ "DEL", "# a\x7f\n::" DOM "1000\n", NTFSMAC_USERMAP_P_CHAR, 1 },
	};
	/* U-UMP-07: what ntfs_build_mapping() and the lookups of acls.c do with the file (review of issue 2):
	 * no mapping without a user and a group; nothing after the implicit entry is reached; repeats. */
	static const pcase engine[] = {
		{ "only a user line (uid, no gid)", "501::" DOM "1001\n", NTFSMAC_USERMAP_P_INCOMPLETE, 0 },
		{ "only user lines", "501::" DOM "1001\n502::" DOM "1002\n", NTFSMAC_USERMAP_P_INCOMPLETE, 0 },
		{ "only a group line", ":20:" DOM "513\n", NTFSMAC_USERMAP_P_INCOMPLETE, 0 },
		{ "only group lines", ":20:" DOM "513\n:21:" DOM "514\n", NTFSMAC_USERMAP_P_INCOMPLETE, 0 },
		{ "implicit line then a user line", "::" DOM "1000\n501:20:" DOM "1001\n", NTFSMAC_USERMAP_P_AFTER_IMPLICIT, 2 },
		{ "implicit line then a comment then a group line", "::" DOM "1000\n# c\n:20:" DOM "513\n",
		  NTFSMAC_USERMAP_P_AFTER_IMPLICIT, 3 },
		{ "two implicit lines", "::" DOM "1000\n::" DOM "2000\n", NTFSMAC_USERMAP_P_AFTER_IMPLICIT, 2 },
		{ "implicit line last is fine", "501:20:" DOM "1001\n::" DOM "1000\n", NTFSMAC_USERMAP_P_OK, 0 },
		{ "repeated uid", "501:20:" DOM "1001\n501:21:" DOM "1002\n", NTFSMAC_USERMAP_P_DUPLICATE, 2 },
		{ "repeated SID", "501:20:" DOM "1001\n502:21:" DOM "1001\n", NTFSMAC_USERMAP_P_DUPLICATE, 2 },
		{ "repeated gid of group-only lines", "501::" DOM "1001\n:20:" DOM "513\n:20:" DOM "514\n",
		  NTFSMAC_USERMAP_P_DUPLICATE, 3 },
		{ "repeated uid, reported at its second line", "501:20:" DOM "1001\n502:21:" DOM "1002\n# c\n501:22:" DOM "1003\n",
		  NTFSMAC_USERMAP_P_DUPLICATE, 4 },
	};
	char big[512], nul[] = "# a\0b\n::S-1-5-21-1-2-3-1000\n";
	unsigned line;
	size_t i;

	run_cases(ok, sizeof(ok) / sizeof(ok[0]));
	run_cases(pattern, sizeof(pattern) / sizeof(pattern[0]));
	run_cases(syntax, sizeof(syntax) / sizeof(syntax[0]));
	run_cases(structure, sizeof(structure) / sizeof(structure[0]));
	run_cases(engine, sizeof(engine) / sizeof(engine[0]));

	/* A NUL byte, and the line-length limit of the engine (LINESZ 120): 120 is read whole. */
	line = 99;
	CHECK_EQ_INT(ntfsmac_usermap_parse_text(nul, sizeof(nul) - 1u, &line), NTFSMAC_USERMAP_P_CHAR);
	CHECK_EQ_INT(line, 1);
	{
		/* The SID and the numbers are capped by the grammar, so no line that is valid otherwise
		 * reaches 120 bytes; the length rule (the engine cuts there) is defence in depth, shown on
		 * a line whose only fault is its length. */
		const char *pre = "501:20:";
		size_t pl = strlen(pre);

		memcpy(big, pre, pl);
		memset(big + pl, '1', NTFSMAC_USERMAP_LINE_MAX + 1u - pl);
		big[NTFSMAC_USERMAP_LINE_MAX + 1u] = '\n';
		line = 99;
		CHECK_EQ_INT(ntfsmac_usermap_parse_text(big, NTFSMAC_USERMAP_LINE_MAX + 2u, &line),
			     NTFSMAC_USERMAP_P_LINE_LONG);
		CHECK_EQ_INT(line, 1);
		/* a comment of any length is fine */
		memset(big, 'x', 400);
		big[0] = '#';
		big[400] = '\n';
		memcpy(big + 401, "::S-1-5-21-1-2-3-1000\n", 22);
		CHECK_EQ_INT(ntfsmac_usermap_parse_text(big, 423, &line), NTFSMAC_USERMAP_P_OK);
	}

	/* Size bound: 1 MiB is read, one byte more is refused before any line is looked at. */
	{
		size_t max = NTFSMAC_USERMAP_MAX_SIZE, pos = 22;
		char *h = malloc(max + 1u);

		CHECK(h != NULL);
		if (h != NULL) {
			memcpy(h, "::S-1-5-21-1-2-3-1000\n", 22);
			while (pos < max) {
				size_t l = max - pos < 100u ? max - pos : 100u;

				memset(h + pos, '#', l);
				h[pos + l - 1u] = '\n';
				pos += l;
			}
			h[max] = '#';
			line = 99;
			CHECK_EQ_INT(ntfsmac_usermap_parse_text(h, max, &line), NTFSMAC_USERMAP_P_OK);
			line = 99;
			CHECK_EQ_INT(ntfsmac_usermap_parse_text(h, max + 1u, &line), NTFSMAC_USERMAP_P_TOO_BIG);
			CHECK_EQ_INT(line, 0);
			free(h);
		}
	}
	CHECK_EQ_INT(ntfsmac_usermap_parse_text(NULL, 5, NULL), NTFSMAC_USERMAP_P_INVAL);
	CHECK_EQ_INT(ntfsmac_usermap_parse_text(NULL, 0, NULL), NTFSMAC_USERMAP_P_EMPTY);

	/* U-UMP-05: the check of a descriptor's fstat alone. */
	{
		ntfsmac_usermap_ident id = good_ident();

		CHECK_EQ_INT(ntfsmac_usermap_stat_verdict(&id, 0), NTFSMAC_USERMAP_V_ENABLE);
		id.mode = S_IFDIR | 0644;
		CHECK_EQ_INT(ntfsmac_usermap_stat_verdict(&id, 0), NTFSMAC_USERMAP_V_NOT_REGULAR);
		id.mode = S_IFIFO | 0644;
		CHECK_EQ_INT(ntfsmac_usermap_stat_verdict(&id, 0), NTFSMAC_USERMAP_V_NOT_REGULAR);
		id.mode = S_IFLNK | 0644;
		CHECK_EQ_INT(ntfsmac_usermap_stat_verdict(&id, 0), NTFSMAC_USERMAP_V_NOT_REGULAR);
		id = good_ident();
		id.uid = 501;
		CHECK_EQ_INT(ntfsmac_usermap_stat_verdict(&id, 0), NTFSMAC_USERMAP_V_NOT_OWNER);
		{
			static const mode_t bad[] = { 0600, 0640, 0664, 0666, 0444, 0755, 04644, 01644, 02644, 0 };

			for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
				id = good_ident();
				id.mode = S_IFREG | bad[i];
				CHECK_EQ_INT(ntfsmac_usermap_stat_verdict(&id, 0), NTFSMAC_USERMAP_V_BAD_MODE);
			}
		}
		id = good_ident();
		id.size = NTFSMAC_USERMAP_MAX_SIZE;
		CHECK_EQ_INT(ntfsmac_usermap_stat_verdict(&id, 0), NTFSMAC_USERMAP_V_ENABLE);
		id.size = (off_t)NTFSMAC_USERMAP_MAX_SIZE + 1;
		CHECK_EQ_INT(ntfsmac_usermap_stat_verdict(&id, 0), NTFSMAC_USERMAP_V_TOO_BIG);
		id.size = -1;
		CHECK_EQ_INT(ntfsmac_usermap_stat_verdict(&id, 0), NTFSMAC_USERMAP_V_TOO_BIG);
		CHECK_EQ_INT(ntfsmac_usermap_stat_verdict(NULL, 0), NTFSMAC_USERMAP_V_NOT_REGULAR);
	}

	/* U-UMP-06: the decision: descriptor, parse, descriptor unchanged, path still the same file. */
	{
		ntfsmac_usermap_ident o = good_ident(), fd = good_ident(), pa = good_ident();
		ntfsmac_usermap_parse ok_p = NTFSMAC_USERMAP_P_OK;

		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, &pa), NTFSMAC_USERMAP_V_ENABLE);
		/* the parse result counts */
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, NTFSMAC_USERMAP_P_PATTERN, &fd, &pa),
			     NTFSMAC_USERMAP_V_PARSE);
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, NTFSMAC_USERMAP_P_EMPTY, &fd, &pa),
			     NTFSMAC_USERMAP_V_PARSE);
		/* the descriptor's own state comes first */
		o.mode = S_IFREG | 0666;
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, NTFSMAC_USERMAP_P_PATTERN, &fd, &pa),
			     NTFSMAC_USERMAP_V_BAD_MODE);
		o = good_ident();
		/* the descriptor changed after the read */
		fd.size++;
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, &pa), NTFSMAC_USERMAP_V_CHANGED);
		fd = good_ident();
		fd.mtime_ns++;
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, &pa), NTFSMAC_USERMAP_V_CHANGED);
		fd = good_ident();
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, NULL, &pa), NTFSMAC_USERMAP_V_CHANGED);
		/* the name now leads elsewhere (the check/use gap) */
		pa.ino++;
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, &pa), NTFSMAC_USERMAP_V_CHANGED);
		pa = good_ident();
		pa.dev++;
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, &pa), NTFSMAC_USERMAP_V_CHANGED);
		pa = good_ident();
		pa.mtime_s++;
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, &pa), NTFSMAC_USERMAP_V_CHANGED);
		pa = good_ident();
		pa.ctime_ns++;
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, &pa), NTFSMAC_USERMAP_V_CHANGED);
		fd = good_ident();
		fd.ctime_s++;
		pa = good_ident();
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, &pa), NTFSMAC_USERMAP_V_CHANGED);
		fd = good_ident();
		pa.size++;
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, &pa), NTFSMAC_USERMAP_V_CHANGED);
		pa = good_ident();
		pa.mode = S_IFREG | 0664;
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, &pa), NTFSMAC_USERMAP_V_CHANGED);
		pa = good_ident();
		pa.uid = 501;
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, &pa), NTFSMAC_USERMAP_V_CHANGED);
		/* the name is gone, or became a link */
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, NULL), NTFSMAC_USERMAP_V_GONE);
		pa = good_ident();
		pa.mode = S_IFLNK | 0777;
		CHECK_EQ_INT(ntfsmac_usermap_decide(&o, 0, ok_p, &fd, &pa), NTFSMAC_USERMAP_V_GONE);
		/* every verdict has a text */
		for (i = 0; i <= NTFSMAC_USERMAP_V_GONE; i++)
			CHECK(strcmp(ntfsmac_usermap_verdict_str((ntfsmac_usermap_verdict)i), "unknown") != 0);
		for (i = 0; i <= NTFSMAC_USERMAP_P_DUPLICATE; i++)
			CHECK(strcmp(ntfsmac_usermap_parse_str((ntfsmac_usermap_parse)i), "unknown") != 0);
	}

	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_usermap: ok\n");
	return 0;
}
