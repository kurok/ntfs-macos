/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Request line builder of the CLI (T-M4-04). Escapes every string. */
#include <string.h>

#include "cli.h"

static void put(ntfsmac_cli_req *r, const char *s, size_t n)
{
	if (r->overflow)
		return;
	/* keep one byte for the closing brace written by req_end() */
	if (r->len + n + 1 > NTFSMAC_CTL_LINE_MAX) {
		r->overflow = true;
		return;
	}
	memcpy(r->buf + r->len, s, n);
	r->len += n;
}

static void put_json_string(ntfsmac_cli_req *r, const char *s)
{
	static const char hex[] = "0123456789abcdef";
	size_t i;

	put(r, "\"", 1);
	for (i = 0; s[i] != '\0'; i++) {
		unsigned char ch = (unsigned char)s[i];

		if (ch == '"' || ch == '\\') {
			char esc[2] = {'\\', (char)ch};

			put(r, esc, 2);
		} else if (ch < 0x20) {
			char esc[6] = {'\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 15]};

			put(r, esc, 6);
		} else {
			put(r, (const char *)&ch, 1);
		}
	}
	put(r, "\"", 1);
}

void ntfsmac_cli_req_begin(ntfsmac_cli_req *r, ntfsmac_ctl_cmd cmd)
{
	memset(r, 0, sizeof *r);
	put(r, "{\"cmd\":", 7);
	put_json_string(r, ntfsmac_ctl_cmd_name(cmd));
}

static void key(ntfsmac_cli_req *r, const char *k)
{
	put(r, ",", 1);
	put_json_string(r, k);
	put(r, ":", 1);
}

void ntfsmac_cli_req_str(ntfsmac_cli_req *r, const char *k, const char *value)
{
	key(r, k);
	put_json_string(r, value);
}

void ntfsmac_cli_req_bool(ntfsmac_cli_req *r, const char *k, bool value)
{
	key(r, k);
	if (value)
		put(r, "true", 4);
	else
		put(r, "false", 5);
}

int ntfsmac_cli_req_end(ntfsmac_cli_req *r)
{
	put(r, "}", 1);
	if (r->overflow)
		return -1;
	r->buf[r->len] = '\0';
	return 0;
}
