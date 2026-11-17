/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-PL-01: the launchd job of the daemon (integration/launchd/org.ntfs-macos.ntfsmacd.plist,
 * T-M4-08). The MachServices entry must name exactly NTFSMAC_CTL_XPC_SERVICE, or the privileged
 * XPC service never receives a message; the program path must be the install path of the
 * manifest. A plain text scan: the file is ours and fixed.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "ntfsmac/ctl_proto.h"

#ifndef NTFSMAC_PLIST_PATH
#error "NTFSMAC_PLIST_PATH must be defined by CMake"
#endif

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = calloc(1, (size_t)n + 1u);
	if (buf != NULL && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	return buf;
}

/* The text of the value element that follows `<key>name</key>`, up to the end of that element. */
static const char *after_key(const char *doc, const char *name)
{
	char key[256];
	const char *p;

	snprintf(key, sizeof key, "<key>%s</key>", name);
	p = strstr(doc, key);
	return p != NULL ? p + strlen(key) : NULL;
}

static bool next_is(const char *p, const char *what)
{
	if (p == NULL)
		return false;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return strncmp(p, what, strlen(what)) == 0;
}

int main(void)
{
	char *doc = slurp(NTFSMAC_PLIST_PATH);
	const char *p;

	CHECK(doc != NULL);
	if (doc == NULL)
		return 1;
	CHECK(next_is(after_key(doc, "Label"), "<string>org.ntfs-macos.ntfsmacd</string>"));
	CHECK(next_is(after_key(doc, "ProgramArguments"), "<array>"));
	CHECK(strstr(doc, "<string>/usr/local/libexec/ntfs-macos/ntfsmacd</string>") != NULL);
	/* The MachServices dictionary holds exactly the privileged service, set to true. */
	p = after_key(doc, "MachServices");
	CHECK(next_is(p, "<dict>"));
	CHECK(next_is(after_key(doc, NTFSMAC_CTL_XPC_SERVICE), "<true/>"));
	CHECK(strstr(doc, "<key>" NTFSMAC_CTL_XPC_SERVICE "</key>") != NULL);
	/* The job runs as root (no UserName) and is kept alive. */
	CHECK(strstr(doc, "<key>UserName</key>") == NULL);
	CHECK(next_is(after_key(doc, "KeepAlive"), "<true/>"));
	CHECK(next_is(after_key(doc, "RunAtLoad"), "<true/>"));
	free(doc);
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
