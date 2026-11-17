/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Administrator authorization for the CLI (T-M4-04, ADR-019 check 4).
 * Obtains the right through Authorization Services (the system shows the
 * password prompt) and returns the external form for the daemon to check.
 * Off macOS it always fails, so the privileged commands cannot run there.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cli.h"

/* ------------------------------------------------------------------ */
/* A call that may never return, with a time limit (U17, finding 2)    */
/* ------------------------------------------------------------------ */

struct timed_call {
	void (*fn)(void *);
	void *arg;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	bool done;
	bool abandoned; /* the caller gave up: the thread frees this record */
};

static void *timed_call_main(void *p)
{
	struct timed_call *t = p;
	bool free_it;

	t->fn(t->arg);
	pthread_mutex_lock(&t->mu);
	t->done = true;
	free_it = t->abandoned;
	pthread_cond_signal(&t->cv);
	pthread_mutex_unlock(&t->mu);
	if (free_it)
		free(t);
	return NULL;
}

bool ntfsmac_cli_run_with_timeout(void (*fn)(void *), void *arg, unsigned timeout_ms)
{
	struct timed_call *t = calloc(1, sizeof *t);
	pthread_t th;
	struct timespec until;
	bool ok = false;
	int r = 0;

	if (t == NULL || fn == NULL)
		return false;
	t->fn = fn;
	t->arg = arg;
	pthread_mutex_init(&t->mu, NULL);
	pthread_cond_init(&t->cv, NULL);
	if (pthread_create(&th, NULL, timed_call_main, t) != 0) {
		free(t);
		return false;
	}
	pthread_detach(th);
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += timeout_ms / 1000u;
	until.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
	if (until.tv_nsec >= 1000000000L) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&t->mu);
	while (!t->done && r != ETIMEDOUT)
		r = pthread_cond_timedwait(&t->cv, &t->mu, &until);
	ok = t->done;
	if (!ok)
		t->abandoned = true; /* the thread is still inside fn(): it will free the record */
	pthread_mutex_unlock(&t->mu);
	if (ok) {
		pthread_mutex_destroy(&t->mu);
		pthread_cond_destroy(&t->cv);
		free(t);
	}
	return ok;
}

#if defined(__APPLE__)
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>

struct ntfsmac_cli_authz {
	AuthorizationRef ref;
};

/* The prompt call runs on its own thread; the job record lives until that thread is done. */
struct prompt_job {
	char prompt[256];
	AuthorizationRef ref;
	AuthorizationExternalForm ext;
	OSStatus st;
};

static void prompt_job_run(void *p)
{
	struct prompt_job *j = p;
	AuthorizationItem right = {NTFSMAC_CTL_RIGHT_NAME, 0, NULL, 0};
	AuthorizationRights rights = {1, &right};
	AuthorizationItem envitem = {kAuthorizationEnvironmentPrompt, strlen(j->prompt), j->prompt, 0};
	AuthorizationEnvironment env = {1, &envitem};

	j->st = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment, kAuthorizationFlagDefaults,
				    &j->ref);
	if (j->st == errAuthorizationSuccess)
		j->st = AuthorizationCopyRights(j->ref, &rights, &env,
						kAuthorizationFlagInteractionAllowed |
							kAuthorizationFlagExtendRights,
						NULL);
	if (j->st == errAuthorizationSuccess)
		j->st = AuthorizationMakeExternalForm(j->ref, &j->ext);
}

int ntfsmac_cli_authz_acquire(const char *prompt, char hex[2 * NTFSMAC_CTL_AUTH_FORM_BYTES + 1],
			      ntfsmac_cli_authz **out, char *err_text, size_t err_cap)
{
	static const char digits[] = "0123456789abcdef";
	ntfsmac_cli_authz *h;
	struct prompt_job *j;
	size_t i;

	*out = NULL;
	h = calloc(1, sizeof *h);
	j = calloc(1, sizeof *j);
	if (h == NULL || j == NULL) {
		free(h);
		free(j);
		snprintf(err_text, err_cap, "out of memory");
		return -1;
	}
	snprintf(j->prompt, sizeof j->prompt, "%s", prompt);
	/* Without a GUI session the call waits for ever (U17): give up after our own limit. */
	if (!ntfsmac_cli_run_with_timeout(prompt_job_run, j, NTFSMAC_CLI_PROMPT_TIMEOUT_S * 1000u)) {
		/* the thread still owns `j`; it is left behind and the program ends soon */
		free(h);
		snprintf(err_text, err_cap,
			 "no answer to the administrator prompt within %d seconds (is there a desktop "
			 "session with a dialog?)",
			 NTFSMAC_CLI_PROMPT_TIMEOUT_S);
		return -1;
	}
	if (j->st != errAuthorizationSuccess) {
		OSStatus st = j->st;

		if (st == errAuthorizationCanceled)
			snprintf(err_text, err_cap, "the administrator prompt was cancelled");
		else if (st == errAuthorizationInteractionNotAllowed)
			snprintf(err_text, err_cap,
				 "no administrator prompt is possible in this session");
		else if (st == errAuthorizationDenied)
			snprintf(err_text, err_cap, "the administrator authorization was denied");
		else
			snprintf(err_text, err_cap, "authorization failed (error %d)", (int)st);
		if (j->ref != NULL)
			AuthorizationFree(j->ref, kAuthorizationFlagDestroyRights);
		free(h);
		free(j);
		return -1;
	}
	h->ref = j->ref;
	for (i = 0; i < NTFSMAC_CTL_AUTH_FORM_BYTES; i++) {
		hex[2 * i] = digits[(unsigned char)j->ext.bytes[i] >> 4];
		hex[2 * i + 1] = digits[(unsigned char)j->ext.bytes[i] & 15];
	}
	hex[2 * NTFSMAC_CTL_AUTH_FORM_BYTES] = '\0';
	memset(&j->ext, 0, sizeof j->ext);
	free(j);
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
