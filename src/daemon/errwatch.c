/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Session channel parser and clean-session proof (T-M4-13).
 *
 * Source of truth: docs/ARCHITECTURE.md section 7 (records, grammar, bounds,
 * positive clean-session proof), ADR-018, and patch 0006 of NTFS-3G, which
 * writes the records.
 *
 * This file is pure: no I/O, no allocation, no clock. The pipe and the
 * reading live in errwatch_io.c. The parser is a state machine over a fixed
 * line buffer, so no input can make it grow.
 *
 * Grammar (one record per line, at most 32 bytes with the newline, bytes
 * 0x20..0x7E only, single spaces, decimal numbers without sign or leading
 * zeros):
 *	S rw | S ro
 *	E <op> <errno>	op is pwrite, write, sync, close or umount
 *	W flush <errno>
 *	U <n>
 * Order: the first record is S; S comes once; nothing comes after U.
 * At most 64 records. Any violation is a protocol error: the session can no
 * longer be proven clean, and the rest of the stream is discarded.
 */
#include <string.h>

#include "internal.h"

#define ERRNO_DIGITS_MAX 5
#define ERRNO_VALUE_MAX 99999u
#define COUNT_DIGITS_MAX 10
#define COUNT_VALUE_MAX 2147483647u /* the worker counts in an int */

/* Parse `n` decimal digits at `s`. No sign, no leading zero (except "0"). */
static bool parse_dec(const char *s, size_t n, size_t max_digits, uint64_t *out)
{
	uint64_t v = 0;
	size_t i;

	if (n == 0 || n > max_digits)
		return false;
	if (n > 1 && s[0] == '0')
		return false;
	for (i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10u + (uint64_t)(s[i] - '0');
	}
	*out = v;
	return true;
}

/* `<word> <errno>` with the word taken from a fixed list. */
static bool parse_op_errno(const char *s, size_t n, const char *const *ops)
{
	const char *sp = memchr(s, ' ', n);
	size_t oplen, i;
	uint64_t e;

	if (sp == NULL)
		return false;
	oplen = (size_t)(sp - s);
	if (!parse_dec(sp + 1, n - oplen - 1u, ERRNO_DIGITS_MAX, &e) || e == 0 || e > ERRNO_VALUE_MAX)
		return false;
	for (i = 0; ops[i] != NULL; i++)
		if (strlen(ops[i]) == oplen && memcmp(ops[i], s, oplen) == 0)
			return true;
	return false;
}

static void protocol_error(ntfsmac_errwatch *w)
{
	w->flags |= NTFSMAC_EW_PROTOCOL_ERROR;
	w->line_len = 0;
	w->line[0] = '\0';
}

/* One complete line (without the newline) is in w->line. */
static void record_done(ntfsmac_errwatch *w)
{
	static const char *const e_ops[] = { "pwrite", "write", "sync", "close", "umount", NULL };
	static const char *const w_ops[] = { "flush", NULL };
	const char *l = w->line;
	size_t n = w->line_len;
	char kind = l[0];
	bool order_ok = true;
	const uint32_t s_any = NTFSMAC_EW_SAW_S_RW | NTFSMAC_EW_SAW_S_RO;

	w->records++;
	if (w->records > NTFSMAC_ERRWATCH_RECORDS_MAX) { /* record 65 and later: overflow */
		protocol_error(w);
		return;
	}
	if (n < 3u || l[1] != ' ') {
		protocol_error(w);
		return;
	}
	if ((w->flags & NTFSMAC_EW_SAW_U) != 0u)
		order_ok = false; /* nothing comes after U */
	if (w->records == 1u && kind != 'S')
		order_ok = false; /* the first record is S */

	switch (kind) {
	case 'S':
		if ((w->flags & s_any) != 0u)
			order_ok = false; /* S at most once */
		if (n == 4u && memcmp(l + 2, "rw", 2) == 0)
			w->flags |= NTFSMAC_EW_SAW_S_RW;
		else if (n == 4u && memcmp(l + 2, "ro", 2) == 0)
			w->flags |= NTFSMAC_EW_SAW_S_RO;
		else
			order_ok = false;
		break;
	case 'E':
		if (!parse_op_errno(l + 2, n - 2u, e_ops))
			order_ok = false;
		w->flags |= NTFSMAC_EW_SAW_E;
		break;
	case 'W':
		if (!parse_op_errno(l + 2, n - 2u, w_ops))
			order_ok = false;
		w->flags |= NTFSMAC_EW_SAW_W;
		break;
	case 'U': {
		uint64_t cnt;

		if ((w->flags & s_any) == 0u)
			order_ok = false; /* U needs an S before it */
		if (!parse_dec(l + 2, n - 2u, COUNT_DIGITS_MAX, &cnt) || cnt > COUNT_VALUE_MAX) {
			order_ok = false;
			break;
		}
		w->flags |= NTFSMAC_EW_SAW_U;
		if (cnt == 0u)
			w->flags |= NTFSMAC_EW_SAW_U0;
		else
			w->flags |= NTFSMAC_EW_SAW_E; /* the worker tried `cnt` E records */
		break;
	}
	default:
		order_ok = false;
		break;
	}
	if (!order_ok) {
		protocol_error(w);
		return;
	}
	w->line_len = 0;
	w->line[0] = '\0';
}

void ntfsmac_errwatch_init(ntfsmac_errwatch *w)
{
	if (w == NULL)
		return;
	w->flags = 0;
	w->records = 0;
	w->line_len = 0;
	w->line[0] = '\0';
}

ntfsmac_dmn_status ntfsmac_errwatch_feed(ntfsmac_errwatch *w, const char *buf,
					 size_t len)
{
	size_t i;

	if (w == NULL || (buf == NULL && len != 0u))
		return NTFSMAC_DMN_E_INVAL;
	if ((w->flags & NTFSMAC_EW_EOF) != 0u && len != 0u)
		protocol_error(w); /* bytes after end-of-file cannot be */
	for (i = 0; i < len; i++) {
		unsigned char c;

		if ((w->flags & NTFSMAC_EW_PROTOCOL_ERROR) != 0u)
			return NTFSMAC_DMN_E_STATE; /* discard the rest */
		c = (unsigned char)buf[i];
		if (c == '\n') {
			record_done(w);
		} else if (c < 0x20u || c > 0x7Eu) {
			protocol_error(w);
		} else if (w->line_len >= NTFSMAC_ERRWATCH_LINE_MAX - 1u) {
			protocol_error(w); /* no room left for the newline */
		} else {
			w->line[w->line_len++] = (char)c;
			w->line[w->line_len] = '\0';
		}
	}
	return (w->flags & NTFSMAC_EW_PROTOCOL_ERROR) != 0u ? NTFSMAC_DMN_E_STATE : NTFSMAC_DMN_OK;
}

void ntfsmac_errwatch_eof(ntfsmac_errwatch *w)
{
	if (w == NULL)
		return;
	if (w->line_len != 0u) /* a line without its newline */
		protocol_error(w);
	w->flags |= NTFSMAC_EW_EOF;
}

bool ntfsmac_errwatch_clean_proof(const ntfsmac_errwatch *w, bool unmount_seen_first,
				  bool re_adopted, int worker_exit_status)
{
	const uint32_t need = NTFSMAC_EW_SAW_S_RW | NTFSMAC_EW_SAW_U | NTFSMAC_EW_SAW_U0 | NTFSMAC_EW_EOF;
	const uint32_t deny = NTFSMAC_EW_SAW_S_RO | NTFSMAC_EW_SAW_E | NTFSMAC_EW_PROTOCOL_ERROR;

	if (w == NULL)
		return false;
	/* S rw first and U 0 last, no E, nothing after U: the parser enforces the order. */
	if ((w->flags & need) != need || (w->flags & deny) != 0u)
		return false;
	if (w->line_len != 0u || w->records < 2u || w->records > NTFSMAC_ERRWATCH_RECORDS_MAX)
		return false;
	return unmount_seen_first && !re_adopted && worker_exit_status == 0;
}
