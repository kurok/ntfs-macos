/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_SANDBOX_TEST_H
#define NTFSMAC_SANDBOX_TEST_H

/*
 * What sandbox_vm (the driver) sets up in the guest and sandbox_stub (the
 * program under the profile) tries to reach. Both are in this directory.
 */

/* A TCP listener on the loopback address. */
#define SBX_TCP_PORT 47311

/* A Unix socket other than the control socket; mode 0666, so only the profile can refuse. */
#define SBX_OTHER_SOCKET "/private/tmp/ntfsmac-sbx-other.sock"

/* A system Mach service that no profile allows; it exists on every macOS. */
#define SBX_OTHER_SERVICE "com.apple.system.opendirectoryd.api"

/* Files the driver makes, both root-owned and mode 0644. */
#define SBX_USERMAPPING_DIR "/usr/local/etc/ntfs-macos"
#define SBX_OTHER_FILE SBX_USERMAPPING_DIR "/other"

#endif /* NTFSMAC_SANDBOX_TEST_H */
