/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VM probe for the checks of ADR-019 that need the operating system (T-M4-04,
 * question U17). Not a ctest. It runs only inside a disposable macOS VM,
 * driven by tests/unit/ctl/vm/u17.sh: it needs the installed right, code
 * signing variants of this very binary, and (for the daemon side) root.
 *
 *   sig-server REQFILE SOCK     listen on SOCK, accept one client, run the real
 *                               check 3 against it, print the answer
 *   sig-client SOCK SECONDS     connect to SOCK and wait
 *   authz-make RIGHT MODE FILE HOLD
 *                               MODE: none (create a reference only), copy
 *                               (copy the right without interaction), prompt
 *                               (copy it with interaction allowed). Writes the
 *                               external form as hex to FILE, holds the
 *                               reference for HOLD seconds, then frees it
 *                               with the rights destroyed (the CLI's behaviour)
 *   authz-check RIGHT HEX       the daemon side: create a reference from the
 *                               form and ask for the right without interaction
 *                               (what real_authz_verify() does); prints the
 *                               OSStatus and the answer of the real function
 */
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ctl_priv.h"

static const char *answer_name(ntfsmac_ctl_sys_answer a)
{
	return a == NTFSMAC_CTL_SYS_YES ? "YES" : a == NTFSMAC_CTL_SYS_NO ? "NO" : "UNAVAILABLE";
}

static int bind_sock(const char *path)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
	unlink(path);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 1) != 0) {
		perror("bind");
		exit(2);
	}
	return fd;
}

static int sig_server(const char *reqfile, const char *sock)
{
	int ls = bind_sock(sock);
	int fd;
	ntfsmac_ctl_peer peer;
	ntfsmac_ctl_sys_real r;

	printf("ready\n");
	fflush(stdout);
	fd = accept(ls, NULL, NULL);
	ntfsmac_ctl_sys_real_init(&r, reqfile, NTFSMAC_CTL_RIGHT_NAME);
	r.allow_any_owner = true; /* the test file belongs to the test user */
	if (fd < 0 || r.sys.peer_from_fd(r.sys.ctx, fd, &peer) != 0) {
		printf("peer_from_fd failed\n");
		return 2;
	}
	printf("peer uid=%d pid=%d audit_token=%d\n", (int)peer.uid, (int)peer.pid,
	       peer.has_audit_token ? 1 : 0);
	printf("client_signed=%s\n", answer_name(r.sys.client_signed(r.sys.ctx, &peer)));
	unlink(sock);
	return 0;
}

static int sig_client(const char *sock, int seconds)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	int i;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
	for (i = 0; i < 50; i++) { /* the server may still be starting */
		if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0)
			break;
		usleep(100000);
	}
	if (i == 50)
		return 2;
	sleep((unsigned)seconds);
	return 0;
}

static int authz_make(const char *right, const char *mode, const char *file, int hold)
{
	AuthorizationRef ref = NULL;
	AuthorizationExternalForm ext;
	AuthorizationItem item = {right, 0, NULL, 0};
	AuthorizationRights rights = {1, &item};
	OSStatus st = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,
					  kAuthorizationFlagDefaults, &ref);
	FILE *f;
	size_t i;

	printf("create=%d\n", (int)st);
	if (st == errAuthorizationSuccess && strcmp(mode, "none") != 0) {
		AuthorizationFlags flags = kAuthorizationFlagExtendRights;

		if (strcmp(mode, "prompt") == 0)
			flags |= kAuthorizationFlagInteractionAllowed;
		st = AuthorizationCopyRights(ref, &rights, kAuthorizationEmptyEnvironment, flags, NULL);
		printf("copy_rights(%s)=%d\n", mode, (int)st);
		if (st != errAuthorizationSuccess) {
			/* still hand out the form: the daemon must refuse it */
			printf("note: continuing with a reference that does not hold the right\n");
		}
	}
	if (AuthorizationMakeExternalForm(ref, &ext) != errAuthorizationSuccess) {
		printf("make_external_form failed\n");
		return 2;
	}
	f = fopen(file, "w");
	for (i = 0; i < sizeof ext.bytes; i++)
		fprintf(f, "%02x", (unsigned char)ext.bytes[i]);
	fprintf(f, "\n");
	fclose(f);
	fflush(stdout);
	sleep((unsigned)hold);
	AuthorizationFree(ref, kAuthorizationFlagDestroyRights);
	printf("freed\n");
	return 0;
}

static int authz_check(const char *right, const char *hex)
{
	AuthorizationExternalForm ext;
	AuthorizationRef ref = NULL;
	AuthorizationItem item = {right, 0, NULL, 0};
	AuthorizationRights rights = {1, &item};
	uint8_t form[NTFSMAC_CTL_AUTH_FORM_BYTES];
	ntfsmac_ctl_sys_real r;
	OSStatus st;
	unsigned i;

	if (strlen(hex) != 2 * sizeof form)
		return 2;
	for (i = 0; i < sizeof form; i++) {
		unsigned v;

		sscanf(hex + 2 * i, "%2x", &v);
		form[i] = (uint8_t)v;
	}
	/* the plain calls, to see the OSStatus (a second reference to the same token) */
	memcpy(ext.bytes, form, sizeof ext.bytes);
	st = AuthorizationCreateFromExternalForm(&ext, &ref);
	printf("create_from_external_form=%d\n", (int)st);
	if (st == errAuthorizationSuccess) {
		st = AuthorizationCopyRights(ref, &rights, kAuthorizationEmptyEnvironment,
					     kAuthorizationFlagExtendRights, NULL);
		printf("copy_rights(no interaction)=%d\n", (int)st);
		AuthorizationFree(ref, kAuthorizationFlagDefaults); /* keep the rights for the real call */
	}
	/* the real function, which frees with the rights destroyed */
	ntfsmac_ctl_sys_real_init(&r, "/unused", right);
	printf("real_authz_verify=%s\n", answer_name(r.sys.authz_verify(r.sys.ctx, form)));
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 4 && strcmp(argv[1], "sig-server") == 0)
		return sig_server(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "sig-client") == 0)
		return sig_client(argv[2], atoi(argv[3]));
	if (argc == 6 && strcmp(argv[1], "authz-make") == 0)
		return authz_make(argv[2], argv[3], argv[4], atoi(argv[5]));
	if (argc == 4 && strcmp(argv[1], "authz-check") == 0)
		return authz_check(argv[2], argv[3]);
	fprintf(stderr, "usage: see the header of ctl_vm_probe.c\n");
	return 2;
}
