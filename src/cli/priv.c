/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The flow shared by `ntfsmac journal-clear` and `ntfsmac repair --clear-dirty` (T-M4-14,
 * docs/ARCHITECTURE.md section 3.4, ADR-017, ADR-019):
 *
 *   1. say what the action does and does not do;
 *   2. collect the typed label and serial (a guard against mistakes, not a boundary);
 *   3. get the administrator authorization. In the product (root fallback of ADR-019, chosen
 *      after U17) the caller must be root (sudo): there is no dialog and no form, and a caller
 *      that is not root is told to run the command with sudo before anything is typed. In a
 *      build without the fallback: a system password dialog, with a time limit of our own
 *      because the call does not fail without a GUI session (U17);
 *   4. send the request on the XPC service, never on the socket, and free the authorization;
 *   5. show the reply.
 *
 * The authorization form goes only to NTFSMAC_CTL_XPC_SERVICE: there is no option to name
 * another service or socket.
 */
#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "json.h"

#define PRIV_LABEL_MAX 255u /* the daemon's limit for confirm_label (decoded bytes) */

int ntfsmac_cli_root_only(void)
{
#if defined(NTFSMAC_CTL_AUTHZ_FALLBACK_ROOT) && NTFSMAC_CTL_AUTHZ_FALLBACK_ROOT
	return 1;
#else
	return 0;
#endif
}

static size_t digits_at(const char *s, size_t *i)
{
	size_t n = 0;

	while (isdigit((unsigned char)s[*i])) {
		(*i)++;
		n++;
	}
	return n;
}

/* "disk" digits, optionally "s" digits (at most 9 each): the daemon checks it again. */
static bool bsd_shape(const char *s)
{
	size_t i = 4, n;

	if (strncmp(s, "disk", 4) != 0)
		return false;
	n = digits_at(s, &i);
	if (n == 0 || n > 9)
		return false;
	if (s[i] == 's') {
		i++;
		n = digits_at(s, &i);
		if (n == 0 || n > 9)
			return false;
	}
	return s[i] == '\0';
}

/* probe 20 s, volmark up to 20 s + 120 s, and the daemon's own work */
#define PRIV_EXCHANGE_TIMEOUT_MS 200000

static void usage_priv(const ntfsmac_cli_ctx *ctx, const char *name, const char *flag)
{
	fprintf(ctx->err, "usage: ntfsmac %s%s%s [--label TEXT --serial HEX8] <bsd>\n", name,
		flag != NULL ? " " : "", flag != NULL ? flag : "");
}

/* Read one line from `in`, without its line end. false at end of file or on an empty line. */
static bool read_line(FILE *in, char *buf, size_t cap)
{
	size_t n;

	if (fgets(buf, (int)cap, in) == NULL)
		return false;
	n = strlen(buf);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return n > 0 && n < cap - 1;
}

static bool serial_shape(char *s)
{
	size_t i;

	if (strlen(s) != 8u)
		return false;
	for (i = 0; s[i] != '\0'; i++) {
		if (!isxdigit((unsigned char)s[i]))
			return false;
		s[i] = (char)toupper((unsigned char)s[i]); /* the daemon wants upper case */
	}
	return true;
}

/* What the daemon found worth a stronger word (section 3.5, path 3 and ADR-017). */
static void print_warning(const ntfsmac_cli_ctx *ctx, const char *reply)
{
	if (strstr(reply, "\"warning\":\"write_error\"") != NULL)
		fputs("Warning: the session that left this entry ended with a write error. Files written "
		      "in it may be damaged. Run Windows chkdsk before you rely on this volume.\n",
		      ctx->err);
	else if (strstr(reply, "\"warning\":\"lost_flush\"") != NULL)
		fputs("Warning: the dirty marker is gone, but nothing shows that this volume was checked "
		      "since this Mac used it. A drive can lose a write when it is unplugged. Windows "
		      "chkdsk is strongly preferred.\n",
		      ctx->err);
	else if (strstr(reply, "\"warning\":\"journal_entry\"") != NULL)
		fputs("Warning: this Mac left the volume open earlier; files written in that session may be "
		      "incomplete or damaged. Windows chkdsk is strongly preferred.\n",
		      ctx->err);
}

/*
 * ADR-017: the specific warning comes BEFORE the typed confirmation and the password. The facts
 * are the `journal` member of the read-only `status` reply (the entry's state, the lost-flush
 * pattern, the time this Mac left the volume open). If the daemon cannot say, only the general
 * text above has been shown; the daemon re-checks everything and repeats the warning in its
 * reply either way.
 */
static void print_entry_facts(const ntfsmac_cli_ctx *ctx, const ntfsmac_cli_priv_hooks *h,
			      const char *bsd)
{
	int (*xchg)(const char *, const char *, int, char *, size_t, char *, size_t) =
		h != NULL && h->sock_exchange != NULL ? h->sock_exchange : ntfsmac_cli_exchange;
	ntfsmac_cli_req req;
	char reply[NTFSMAC_CTL_REPLY_MAX], why[128];
	json_doc *doc;
	const json_node *root, *jn;
	const char *warning, *when;

	ntfsmac_cli_req_begin(&req, NTFSMAC_CTL_CMD_STATUS);
	ntfsmac_cli_req_str(&req, "bsd", bsd);
	if (ntfsmac_cli_req_end(&req) != 0 ||
	    xchg(ctx->socket_path, req.buf, 10000, reply, sizeof reply, why, sizeof why) != 0)
		return;
	doc = json_parse(reply, strlen(reply));
	root = json_root(doc);
	jn = root != NULL ? json_member(root, "journal") : NULL;
	if (jn == NULL || jn->type != JSON_OBJECT) {
		json_free(doc);
		return;
	}
	warning = json_member_str(jn, "warning");
	when = json_member_str(jn, "left_open");
	if (when != NULL) {
		fputs("This Mac left the volume open on ", ctx->err);
		ntfsmac_cli_print_safe(ctx->err, when);
		fputs(" (UTC); files written in that session may be incomplete or damaged. Windows "
		      "chkdsk is strongly preferred.\n",
		      ctx->err);
	}
	if (warning != NULL && strcmp(warning, "write_error") == 0)
		fputs("Warning: that session ended with a write error. Files written in it may be damaged.\n",
		      ctx->err);
	else if (warning != NULL && strcmp(warning, "lost_flush") == 0)
		fputs("Warning: the dirty marker is gone, but nothing shows that this volume was checked "
		      "since this Mac used it (a drive can lose a write when it is unplugged).\n",
		      ctx->err);
	json_free(doc);
}

int ntfsmac_cli_priv_run(const ntfsmac_cli_ctx *ctx, ntfsmac_ctl_cmd cmd, const char *action,
			 int argc, char **argv)
{
	const ntfsmac_cli_priv_hooks *h = ctx->hooks;
	const char *name = cmd == NTFSMAC_CTL_CMD_JOURNAL_CLEAR ? "journal-clear" : "repair";
	const char *flag = action != NULL ? "--clear-dirty" : NULL;
	const char *label_arg = NULL, *serial_arg = NULL, *bsd = NULL;
	char label[PRIV_LABEL_MAX + 2];
	char serial[16];
	char hex[2 * NTFSMAC_CTL_AUTH_FORM_BYTES + 1];
	char why[160];
	char reply[NTFSMAC_CTL_REPLY_MAX];
	ntfsmac_cli_req req;
	ntfsmac_cli_authz *az = NULL;
	FILE *in = (h != NULL && h->in != NULL) ? h->in : stdin;
	bool root = h != NULL && h->is_root != NULL ? h->is_root() != 0 : geteuid() == 0;
	bool root_only = h != NULL && h->root_only != NULL ? h->root_only() != 0
							   : ntfsmac_cli_root_only() != 0;
	int rc, i;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--label") == 0 && i + 1 < argc && label_arg == NULL)
			label_arg = argv[++i];
		else if (strcmp(argv[i], "--serial") == 0 && i + 1 < argc && serial_arg == NULL)
			serial_arg = argv[++i];
		else if (argv[i][0] != '-' && bsd == NULL)
			bsd = argv[i];
		else {
			usage_priv(ctx, name, flag);
			return NTFSMAC_CLI_EXIT_USAGE;
		}
	}
	if (bsd == NULL || !bsd_shape(bsd)) {
		fputs("ntfsmac: a BSD name is needed, for example disk4s1.\n", ctx->err);
		usage_priv(ctx, name, flag);
		return NTFSMAC_CLI_EXIT_USAGE;
	}

	/* The daemon would refuse a caller that is not root: say so before anything is typed. */
	if (root_only && !root) {
		fprintf(ctx->err,
			"ntfsmac: %s needs administrator rights. Run it again with sudo: "
			"sudo ntfsmac %s%s%s %s\n",
			name, name, flag != NULL ? " " : "", flag != NULL ? flag : "", bsd);
		return NTFSMAC_CLI_EXIT_FAILED;
	}

	/* 1. What it does and what it does not do (ADR-017). */
	if (cmd == NTFSMAC_CTL_CMD_JOURNAL_CLEAR)
		fputs("This removes the record that this Mac left the volume open, so that the next mount "
		      "can be read/write again. It does not check or repair the file system. If the "
		      "earlier session ended unclean, files written in it may be incomplete or damaged; "
		      "Windows chkdsk is strongly preferred. The volume's current mount is not changed.\n",
		      ctx->err);
	else
		fputs("This clears the dirty flag on the volume. It does not check or repair the file "
		      "system. If this Mac left the volume open earlier, files written in that session may "
		      "be incomplete or damaged; Windows chkdsk is strongly preferred. The volume must not "
		      "be mounted.\n",
		      ctx->err);

	print_entry_facts(ctx, h, bsd);

	/* 2. The typed confirmation. */
	if (label_arg != NULL) {
		if (strlen(label_arg) > PRIV_LABEL_MAX) {
			fputs("ntfsmac: the label is too long.\n", ctx->err);
			return NTFSMAC_CLI_EXIT_USAGE;
		}
		snprintf(label, sizeof label, "%s", label_arg);
	} else {
		fputs("Type the volume label exactly as `ntfsmac list` shows it: ", ctx->err);
		fflush(ctx->err);
		if (!read_line(in, label, sizeof label) || strlen(label) > PRIV_LABEL_MAX) {
			fputs("\nntfsmac: no label was typed; nothing was done.\n", ctx->err);
			return NTFSMAC_CLI_EXIT_USAGE;
		}
	}
	if (serial_arg != NULL) {
		snprintf(serial, sizeof serial, "%s", serial_arg);
	} else {
		fputs("Type the last 8 hex digits of the volume serial: ", ctx->err);
		fflush(ctx->err);
		if (!read_line(in, serial, sizeof serial)) {
			fputs("\nntfsmac: no serial was typed; nothing was done.\n", ctx->err);
			return NTFSMAC_CLI_EXIT_USAGE;
		}
	}
	if (!serial_shape(serial)) {
		fputs("ntfsmac: the serial is the last 8 hex digits, for example 1A2B3C4D.\n", ctx->err);
		return NTFSMAC_CLI_EXIT_USAGE;
	}

	/* 3. Administrator authorization. */
	hex[0] = '\0';
	if (root) {
		if (!root_only)
			fputs("ntfsmac: running as root, so there is no password dialog. This daemon "
			      "expects the dialog and may refuse the request.\n",
			      ctx->err);
	} else {
		int (*acquire)(const char *, char *, ntfsmac_cli_authz **, char *, size_t) =
			h != NULL && h->authz_acquire != NULL ? h->authz_acquire
							      : ntfsmac_cli_authz_acquire;

		fprintf(ctx->err,
			"ntfsmac: a system dialog will ask for an administrator password (waiting up to %d "
			"seconds). If none appears, run this command from a terminal in the logged-in "
			"desktop session.\n",
			NTFSMAC_CLI_PROMPT_TIMEOUT_S);
		fflush(ctx->err);
		if (acquire(cmd == NTFSMAC_CTL_CMD_JOURNAL_CLEAR
				    ? "ntfsmac wants to clear the session journal entry of a volume."
				    : "ntfsmac wants to clear the dirty flag of a volume.",
			    hex, &az, why, sizeof why) != 0) {
			fprintf(ctx->err, "ntfsmac: %s.\n", why);
			return NTFSMAC_CLI_EXIT_FAILED;
		}
	}

	/* 4. The request, on the XPC service only. */
	ntfsmac_cli_req_begin(&req, cmd);
	ntfsmac_cli_req_str(&req, "bsd", bsd);
	if (action != NULL)
		ntfsmac_cli_req_str(&req, "action", action);
	ntfsmac_cli_req_str(&req, "confirm_label", label);
	ntfsmac_cli_req_str(&req, "confirm_serial", serial);
	if (!root)
		ntfsmac_cli_req_str(&req, "auth", hex);
	if (ntfsmac_cli_req_end(&req) != 0) {
		fputs("ntfsmac: the request is too long.\n", ctx->err);
		rc = NTFSMAC_CLI_EXIT_USAGE;
		goto done;
	}
	{
		int (*xchg)(const char *, const char *, int, char *, size_t, char *, size_t) =
			h != NULL && h->exchange != NULL ? h->exchange
							 : ntfsmac_cli_privileged_exchange;

		if (xchg(NTFSMAC_CTL_XPC_SERVICE, req.buf, PRIV_EXCHANGE_TIMEOUT_MS, reply,
			 sizeof reply, why, sizeof why) != 0) {
			fprintf(ctx->err, "ntfsmac: %s.\n", why);
			rc = NTFSMAC_CLI_EXIT_UNAVAILABLE;
			goto done;
		}
	}
	/* 5. Show it. */
	rc = ntfsmac_cli_render(ctx, cmd, reply);
	if (rc == NTFSMAC_CLI_EXIT_OK)
		print_warning(ctx, reply);
done:
	memset(hex, 0, sizeof hex);
	memset(req.buf, 0, sizeof req.buf); /* the request holds the form */
	if (az != NULL) {
		void (*release)(ntfsmac_cli_authz *) =
			h != NULL && h->authz_release != NULL ? h->authz_release
							      : ntfsmac_cli_authz_release;

		release(az);
	}
	return rc;
}
