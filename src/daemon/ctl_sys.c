/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The real system facts behind the peer checks (T-M4-04, ADR-019).
 *
 * This is the one file besides da.c and main.c that includes Apple
 * framework headers: code-signing and Authorization Services are in
 * Security.framework, which brings CoreFoundation types. Nothing from
 * here leaks into the other ctl files; they see only ntfsmac_ctl_sys.
 *
 * Off macOS the file builds a fail-closed object: the peer comes from
 * SO_PEERCRED (for the portable build and its tests), and signature and
 * authorization answer UNAVAILABLE, so a privileged request is refused.
 *
 * Nothing in this file can be exercised on the build host without root and
 * the installed right; T-M4-04 runs it in a disposable VM (U17).
 */
#if !defined(__APPLE__)
#define _GNU_SOURCE /* struct ucred for SO_PEERCRED */
#endif
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "ctl_priv.h"

#if defined(__APPLE__)
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <bsm/audit.h>
#include <libproc.h>
#include <membership.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/ucred.h>

#ifndef SOL_LOCAL
#define SOL_LOCAL 0
#endif
#ifndef LOCAL_PEERTOKEN
#define LOCAL_PEERTOKEN 0x006
#endif
/* Kernel code-signing flag: hardened runtime (CS_RUNTIME in <kern/cs_blobs.h>). */
#define NTFSMAC_CS_RUNTIME 0x00010000u
#endif

/* ------------------------------------------------------------------ */
/* Peer identity                                                       */
/* ------------------------------------------------------------------ */

/* Owner of /dev/console is the user at the console; root means nobody is logged in. */
static bool is_console_user(uid_t uid)
{
	struct stat st;

	if (stat("/dev/console", &st) != 0)
		return false;
	return st.st_uid != 0 && st.st_uid == uid;
}

#if defined(__APPLE__)
static bool is_admin(uid_t uid)
{
	uuid_t user_uuid, group_uuid;
	struct group *gr = getgrnam("admin");
	int member = 0;

	if (gr == NULL)
		return false;
	if (mbr_uid_to_uuid(uid, user_uuid) != 0 || mbr_gid_to_uuid(gr->gr_gid, group_uuid) != 0)
		return false;
	if (mbr_check_membership(user_uuid, group_uuid, &member) != 0)
		return false;
	return member != 0;
}

static int real_peer_from_fd(void *ctx, int fd, ntfsmac_ctl_peer *out)
{
	struct xucred cred;
	socklen_t len = sizeof cred;
	pid_t pid = 0;
	audit_token_t token;
	socklen_t tlen = sizeof token;
	socklen_t plen = sizeof pid;

	(void)ctx;
	memset(out, 0, sizeof *out);
	if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &cred, &len) != 0 ||
	    cred.cr_version != XUCRED_VERSION)
		return -1;
	if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &plen) != 0 || pid <= 0)
		return -1;
	out->uid = cred.cr_uid;
	out->pid = pid;
	out->console_user = is_console_user(out->uid);
	out->admin = is_admin(out->uid);
	if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &tlen) == 0 &&
	    tlen == sizeof token && (pid_t)token.val[5] == pid /* pid is word 5 of audit_token_t */) {
		memcpy(out->audit_token, &token, sizeof token);
		out->has_audit_token = true;
	}
	return 0;
}

static ntfsmac_ctl_sys_proc real_proc_parent(void *ctx, pid_t pid, pid_t *ppid, pid_t *pgid)
{
	struct proc_bsdinfo info;
	int n;

	(void)ctx;
	errno = 0;
	n = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, (int)sizeof info);
	if (n != (int)sizeof info)
		return (n <= 0 && errno == ESRCH) ? NTFSMAC_CTL_PROC_GONE : NTFSMAC_CTL_PROC_ERROR;
	/* The kernel answers for a recycled pid too; a zombie has pbi_status SZOMB. */
	if (info.pbi_status == SZOMB)
		return NTFSMAC_CTL_PROC_GONE;
	*ppid = (pid_t)info.pbi_ppid;
	*pgid = (pid_t)info.pbi_pgid;
	return NTFSMAC_CTL_PROC_OK;
}
#else /* not macOS: portable build, fail closed */
static int real_peer_from_fd(void *ctx, int fd, ntfsmac_ctl_peer *out)
{
	struct ucred cred;
	socklen_t len = sizeof cred;

	(void)ctx;
	memset(out, 0, sizeof *out);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
		return -1;
	out->uid = cred.uid;
	out->pid = cred.pid;
	out->console_user = is_console_user(out->uid);
	out->admin = out->uid == 0;
	return 0;
}

static ntfsmac_ctl_sys_proc real_proc_parent(void *ctx, pid_t pid, pid_t *ppid, pid_t *pgid)
{
	(void)ctx;
	(void)pid;
	(void)ppid;
	(void)pgid;
	return NTFSMAC_CTL_PROC_ERROR;
}
#endif

/* ------------------------------------------------------------------ */
/* Check 3: code signature of the peer                                 */
/* ------------------------------------------------------------------ */

#if defined(__APPLE__)
/* Read the one-line requirement from the installer's file, with the same owner rules as a config. */
static bool read_requirement(const ntfsmac_ctl_sys_real *r, char *buf, size_t cap)
{
	struct stat st;
	ssize_t n;
	int fd = open(r->requirement_file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

	if (fd < 0)
		return false;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    (size_t)st.st_size >= cap || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
	    (st.st_uid != 0 && !r->allow_any_owner)) {
		close(fd);
		return false;
	}
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n <= 0)
		return false;
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return n > 0;
}

static ntfsmac_ctl_sys_answer real_client_signed(void *ctx, const ntfsmac_ctl_peer *peer)
{
	const ntfsmac_ctl_sys_real *r = ctx;
	char text[1024];
	audit_token_t token;
	CFDataRef tok = NULL;
	CFDictionaryRef attrs = NULL;
	CFStringRef reqtext = NULL;
	SecCodeRef code = NULL;
	SecRequirementRef req = NULL;
	CFDictionaryRef info = NULL;
	ntfsmac_ctl_sys_answer answer = NTFSMAC_CTL_SYS_UNAVAILABLE;
	OSStatus st;

	if (!peer->has_audit_token || !read_requirement(r, text, sizeof text))
		return NTFSMAC_CTL_SYS_UNAVAILABLE;
	memcpy(&token, peer->audit_token, sizeof token);

	tok = CFDataCreate(NULL, (const UInt8 *)&token, sizeof token);
	if (tok == NULL)
		goto out;
	{
		const void *keys[1] = {kSecGuestAttributeAudit};
		const void *vals[1] = {tok};

		attrs = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
					   &kCFTypeDictionaryValueCallBacks);
	}
	reqtext = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
	if (attrs == NULL || reqtext == NULL)
		goto out;
	if (SecCodeCopyGuestWithAttributes(NULL, attrs, kSecCSDefaultFlags, &code) != errSecSuccess)
		goto out;
	if (SecRequirementCreateWithString(reqtext, kSecCSDefaultFlags, &req) != errSecSuccess)
		goto out; /* a broken requirement file is "cannot tell", not "unsigned" */

	st = SecCodeCheckValidity(code, kSecCSDefaultFlags, req);
	if (st != errSecSuccess) {
		answer = NTFSMAC_CTL_SYS_NO;
		goto out;
	}
	/* The live kernel flags of the process must include the hardened runtime. */
	if (SecCodeCopySigningInformation((SecStaticCodeRef)code,
					  kSecCSDynamicInformation | kSecCSSigningInformation,
					  &info) != errSecSuccess || info == NULL)
		goto out;
	{
		CFNumberRef status = CFDictionaryGetValue(info, kSecCodeInfoStatus);
		int32_t flags = 0;

		if (status == NULL || CFGetTypeID(status) != CFNumberGetTypeID() ||
		    !CFNumberGetValue(status, kCFNumberSInt32Type, &flags))
			goto out;
		answer = ((uint32_t)flags & NTFSMAC_CS_RUNTIME) != 0 ? NTFSMAC_CTL_SYS_YES
								     : NTFSMAC_CTL_SYS_NO;
	}
out:
	if (info != NULL)
		CFRelease(info);
	if (req != NULL)
		CFRelease(req);
	if (code != NULL)
		CFRelease(code);
	if (reqtext != NULL)
		CFRelease(reqtext);
	if (attrs != NULL)
		CFRelease(attrs);
	if (tok != NULL)
		CFRelease(tok);
	return answer;
}

/* ------------------------------------------------------------------ */
/* Check 4: the right, validated by the daemon itself                  */
/* ------------------------------------------------------------------ */

static ntfsmac_ctl_sys_answer real_authz_verify(void *ctx,
						const uint8_t form[NTFSMAC_CTL_AUTH_FORM_BYTES])
{
	const ntfsmac_ctl_sys_real *r = ctx;
	AuthorizationExternalForm ext;
	AuthorizationRef ref = NULL;
	AuthorizationItem item;
	AuthorizationRights rights;
	OSStatus st;
	ntfsmac_ctl_sys_answer answer;

	_Static_assert(sizeof ext.bytes == NTFSMAC_CTL_AUTH_FORM_BYTES,
		       "external form size");
	memcpy(ext.bytes, form, sizeof ext.bytes);
	if (AuthorizationCreateFromExternalForm(&ext, &ref) != errAuthorizationSuccess)
		return NTFSMAC_CTL_SYS_NO; /* a form the system does not know */

	memset(&item, 0, sizeof item);
	item.name = r->right_name;
	rights.count = 1;
	rights.items = &item;
	/*
	 * No interaction flag: the daemon must never show a prompt. The right
	 * is granted only if this reference already holds it from the CLI's
	 * prompt (own definition of the right: class user, group admin,
	 * shared false, timeout 0).
	 */
	st = AuthorizationCopyRights(ref, &rights, kAuthorizationEmptyEnvironment,
				     kAuthorizationFlagExtendRights, NULL);
	if (st == errAuthorizationSuccess)
		answer = NTFSMAC_CTL_SYS_YES;
	else if (st == errAuthorizationDenied || st == errAuthorizationCanceled ||
		 st == errAuthorizationInteractionNotAllowed)
		answer = NTFSMAC_CTL_SYS_NO;
	else
		answer = NTFSMAC_CTL_SYS_UNAVAILABLE;
	/*
	 * This is not a single-use form. Freeing our reference does not end the
	 * CLI's: U17 showed that a second check of the same form still passes
	 * while the CLI holds its reference. The form stays valid until the CLI
	 * frees that reference, and the replay table (ctl_peer.c) is what stops
	 * a second use. The table is lost when the daemon restarts.
	 */
	AuthorizationFree(ref, kAuthorizationFlagDestroyRights);
	return answer;
}

/*
 * The stored definition of the right, read with AuthorizationRightGet. The
 * package installs it (class user, group admin, shared false, timeout 0).
 * Anything that could let a weaker grant satisfy the daemon is a difference:
 * another class, group, sharing or timeout, a `rule` list that points at
 * another right, `allow-root` or `session-owner` set, or `authenticate-user`
 * switched off. Keys that do not change who may get the right (comment,
 * version, created, modified, tries and so on) are ignored.
 */
static bool dict_is_string(CFDictionaryRef d, CFStringRef key, const char *want)
{
	CFTypeRef v = CFDictionaryGetValue(d, key);
	char buf[32];

	if (v == NULL || CFGetTypeID(v) != CFStringGetTypeID())
		return false;
	if (!CFStringGetCString((CFStringRef)v, buf, sizeof buf, kCFStringEncodingUTF8))
		return false;
	return strcmp(buf, want) == 0;
}

/* The key must be present and equal to `want`. */
static bool dict_is_bool(CFDictionaryRef d, CFStringRef key, bool want)
{
	CFTypeRef v = CFDictionaryGetValue(d, key);

	if (v == NULL || CFGetTypeID(v) != CFBooleanGetTypeID())
		return false;
	return (CFBooleanGetValue((CFBooleanRef)v) != 0) == want;
}

/* An absent key counts as `dflt`. */
static bool dict_bool_or(CFDictionaryRef d, CFStringRef key, bool dflt)
{
	CFTypeRef v = CFDictionaryGetValue(d, key);

	if (v == NULL)
		return dflt;
	if (CFGetTypeID(v) != CFBooleanGetTypeID())
		return !dflt; /* not a boolean: treat as the unsafe value */
	return CFBooleanGetValue((CFBooleanRef)v) != 0;
}

static ntfsmac_ctl_sys_answer real_right_definition(void *ctx)
{
	const ntfsmac_ctl_sys_real *r = ctx;
	CFDictionaryRef def = NULL;
	ntfsmac_ctl_sys_answer answer = NTFSMAC_CTL_SYS_NO;
	OSStatus st;

	st = AuthorizationRightGet(r->right_name, &def);
	if (st == errAuthorizationDenied || (st == errAuthorizationSuccess && def == NULL))
		return NTFSMAC_CTL_SYS_NO; /* no such right */
	if (st != errAuthorizationSuccess)
		return NTFSMAC_CTL_SYS_UNAVAILABLE;

	if (CFGetTypeID(def) == CFDictionaryGetTypeID() &&
	    dict_is_string(def, CFSTR("class"), "user") &&
	    dict_is_string(def, CFSTR("group"), "admin") &&
	    dict_is_bool(def, CFSTR("shared"), false) &&
	    CFDictionaryGetValue(def, CFSTR("rule")) == NULL &&
	    !dict_bool_or(def, CFSTR("allow-root"), false) &&
	    !dict_bool_or(def, CFSTR("session-owner"), false) &&
	    dict_bool_or(def, CFSTR("authenticate-user"), true)) {
		CFTypeRef t = CFDictionaryGetValue(def, CFSTR("timeout"));
		int64_t timeout = -1;

		if (t != NULL && CFGetTypeID(t) == CFNumberGetTypeID() &&
		    CFNumberGetValue((CFNumberRef)t, kCFNumberSInt64Type, &timeout) && timeout == 0)
			answer = NTFSMAC_CTL_SYS_YES;
	}
	CFRelease(def);
	return answer;
}
#else /* not macOS */
static ntfsmac_ctl_sys_answer real_right_definition(void *ctx)
{
	(void)ctx;
	return NTFSMAC_CTL_SYS_UNAVAILABLE;
}

static ntfsmac_ctl_sys_answer real_client_signed(void *ctx, const ntfsmac_ctl_peer *peer)
{
	(void)ctx;
	(void)peer;
	return NTFSMAC_CTL_SYS_UNAVAILABLE;
}

static ntfsmac_ctl_sys_answer real_authz_verify(void *ctx,
						const uint8_t form[NTFSMAC_CTL_AUTH_FORM_BYTES])
{
	(void)ctx;
	(void)form;
	return NTFSMAC_CTL_SYS_UNAVAILABLE;
}
#endif

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

void ntfsmac_ctl_sys_real_init(ntfsmac_ctl_sys_real *r, const char *requirement_file,
			       const char *right_name)
{
	memset(r, 0, sizeof *r);
	r->requirement_file = requirement_file;
	r->right_name = right_name;
	r->sys.ctx = r;
	r->sys.peer_from_fd = real_peer_from_fd;
	r->sys.proc_parent = real_proc_parent;
	r->sys.client_signed = real_client_signed;
	r->sys.authz_verify = real_authz_verify;
	r->sys.right_definition = real_right_definition;
}

static ntfsmac_ctl_sys_real g_default;
static pthread_once_t g_default_once = PTHREAD_ONCE_INIT;

static void default_init(void)
{
	ntfsmac_ctl_sys_real_init(&g_default, NTFSMAC_CTL_REQUIREMENT_FILE, NTFSMAC_CTL_RIGHT_NAME);
}

const ntfsmac_ctl_sys *ntfsmac_ctl_sys_default(void)
{
	pthread_once(&g_default_once, default_init);
	return &g_default.sys;
}
