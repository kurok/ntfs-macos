/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Administrator authorization for the CLI (T-M4-04, ADR-019 check 4).
 * Obtains the right through Authorization Services (the system shows the
 * password prompt) and returns the external form for the daemon to check.
 * Off macOS it always fails, so the privileged commands cannot run there.
 */
#include <stdlib.h>
#include <string.h>

#include "cli.h"

#if defined(__APPLE__)
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>

struct ntfsmac_cli_authz {
	AuthorizationRef ref;
};

int ntfsmac_cli_authz_acquire(const char *prompt, char hex[2 * NTFSMAC_CTL_AUTH_FORM_BYTES + 1],
			      ntfsmac_cli_authz **out, char *err_text, size_t err_cap)
{
	static const char digits[] = "0123456789abcdef";
	ntfsmac_cli_authz *h;
	AuthorizationExternalForm ext;
	AuthorizationItem right = {NTFSMAC_CTL_RIGHT_NAME, 0, NULL, 0};
	AuthorizationRights rights = {1, &right};
	AuthorizationItem envitem = {kAuthorizationEnvironmentPrompt, strlen(prompt), (void *)prompt, 0};
	AuthorizationEnvironment env = {1, &envitem};
	OSStatus st;
	size_t i;

	*out = NULL;
	h = calloc(1, sizeof *h);
	if (h == NULL) {
		snprintf(err_text, err_cap, "out of memory");
		return -1;
	}
	st = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment, kAuthorizationFlagDefaults,
				 &h->ref);
	if (st == errAuthorizationSuccess)
		st = AuthorizationCopyRights(h->ref, &rights, &env,
					     kAuthorizationFlagInteractionAllowed |
						     kAuthorizationFlagExtendRights,
					     NULL);
	if (st == errAuthorizationSuccess)
		st = AuthorizationMakeExternalForm(h->ref, &ext);
	if (st != errAuthorizationSuccess) {
		if (st == errAuthorizationCanceled)
			snprintf(err_text, err_cap, "the administrator prompt was cancelled");
		else if (st == errAuthorizationInteractionNotAllowed)
			snprintf(err_text, err_cap,
				 "no administrator prompt is possible in this session");
		else if (st == errAuthorizationDenied)
			snprintf(err_text, err_cap, "the administrator authorization was denied");
		else
			snprintf(err_text, err_cap, "authorization failed (error %d)", (int)st);
		if (h->ref != NULL)
			AuthorizationFree(h->ref, kAuthorizationFlagDestroyRights);
		free(h);
		return -1;
	}
	for (i = 0; i < NTFSMAC_CTL_AUTH_FORM_BYTES; i++) {
		hex[2 * i] = digits[(unsigned char)ext.bytes[i] >> 4];
		hex[2 * i + 1] = digits[(unsigned char)ext.bytes[i] & 15];
	}
	hex[2 * NTFSMAC_CTL_AUTH_FORM_BYTES] = '\0';
	*out = h;
	return 0;
}

void ntfsmac_cli_authz_release(ntfsmac_cli_authz *h)
{
	if (h == NULL)
		return;
	AuthorizationFree(h->ref, kAuthorizationFlagDestroyRights);
	free(h);
}
#else /* not macOS */
int ntfsmac_cli_authz_acquire(const char *prompt, char hex[2 * NTFSMAC_CTL_AUTH_FORM_BYTES + 1],
			      ntfsmac_cli_authz **out, char *err_text, size_t err_cap)
{
	(void)prompt;
	*out = NULL;
	hex[0] = '\0';
	snprintf(err_text, err_cap, "administrator authorization is only available on macOS");
	return -1;
}

void ntfsmac_cli_authz_release(ntfsmac_cli_authz *h)
{
	(void)h;
}
#endif
