# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-10, T-M6-01, T-M6-02, T-M6-04, T-M6-05: host-side checks of the packaging: the manifest and
# payload lints, the rules of the root-run package scripts, shellcheck, the dry run of the signing script,
# the source release and the comparison of the reproducibility check. They read files, run scripts in
# dry-run or plan mode, or build throw-away packages or repositories in a temporary directory; nothing
# is installed or signed on the host. The installers and the uninstaller run in a disposable macOS guest
# (I-PKG-01..07, tests/integration/packaging; I-PKG-07 is scripts/repro-check.sh).

find_program(NTFSMAC_SH sh)
if(NTFSMAC_SH)
  # U-PKG-01: format, owner, mode, allowed paths, agreement with the code and the shipped files.
  add_test(NAME packaging.manifest
           COMMAND ${NTFSMAC_SH} ${CMAKE_CURRENT_SOURCE_DIR}/manifest_lint.sh ${PROJECT_SOURCE_DIR})
  # U-PKG-02: the lint refuses altered manifests.
  add_test(NAME packaging.manifest_mutants
           COMMAND ${NTFSMAC_SH} ${CMAKE_CURRENT_SOURCE_DIR}/manifest_lint_test.sh
                   ${CMAKE_CURRENT_SOURCE_DIR}/manifest_lint.sh ${PROJECT_SOURCE_DIR})
  # U-PKG-03 (T-M6-01): the release payload plan, payload-check.sh, and the rules of the root-run
  # package scripts (preinstall, postinstall).
  add_test(NAME packaging.payload
           COMMAND ${NTFSMAC_SH} ${CMAKE_CURRENT_SOURCE_DIR}/payload_lint.sh ${PROJECT_SOURCE_DIR})
  # U-PKG-05 (T-M6-02): argument handling and dry run of the signing script (no signing, no keychain).
  add_test(NAME packaging.sign_and_notarize
           COMMAND ${NTFSMAC_SH} ${CMAKE_CURRENT_SOURCE_DIR}/sign_and_notarize_test.sh
                   ${PROJECT_SOURCE_DIR}/packaging/sign-and-notarize.sh)
  # U-PKG-06 (T-M6-05): the GPL source release archive, in a throw-away git repository with a fake
  # tarball (nothing is downloaded).
  add_test(NAME packaging.source_release
           COMMAND ${NTFSMAC_SH} ${CMAKE_CURRENT_SOURCE_DIR}/source_release_test.sh ${PROJECT_SOURCE_DIR})
  # U-PKG-08 (audit M6-F01): the root-only check is one text in the four root-run scripts, walks the
  # path without following a link, and its mutants fail (the cases on a real tree and the mutants run on
  # macOS only; the identity lint of the four copies and the caller checks run everywhere).
  add_test(NAME packaging.root_only
           COMMAND ${NTFSMAC_SH} ${CMAKE_CURRENT_SOURCE_DIR}/root_only_test.sh ${PROJECT_SOURCE_DIR})
  # U-PKG-09 (audit M6-F07): bom-check.sh compares the owners and modes of a built component's Bom
  # with the manifest (small packages made by pkgbuild here; nothing is installed). macOS only:
  # pkgbuild and lsbom exist nowhere else.
  if(APPLE)
    add_test(NAME packaging.bom_check
             COMMAND ${NTFSMAC_SH} ${CMAKE_CURRENT_SOURCE_DIR}/bom_check_test.sh ${PROJECT_SOURCE_DIR})
  endif()
  # The scripts at least parse (they are run only in the guest).
  foreach(s dev-install uninstall build-pkg payload-check bom-check sign-and-notarize)
    add_test(NAME packaging.syntax_${s}
             COMMAND ${NTFSMAC_SH} -n ${PROJECT_SOURCE_DIR}/packaging/${s}.sh)
  endforeach()
  add_test(NAME packaging.syntax_make_source_release
           COMMAND ${NTFSMAC_SH} -n ${PROJECT_SOURCE_DIR}/scripts/make-source-release.sh)
  foreach(s preinstall postinstall)
    add_test(NAME packaging.syntax_${s}
             COMMAND ${NTFSMAC_SH} -n ${PROJECT_SOURCE_DIR}/packaging/scripts/${s})
  endforeach()
  # U-PKG-07 (T-M6-04): the comparison and allow-list of the reproducibility check, with small packages
  # made by pkgbuild here (the real check builds in two guest clones).
  add_test(NAME packaging.repro_check
           COMMAND ${NTFSMAC_SH} ${CMAKE_CURRENT_SOURCE_DIR}/repro_check_test.sh
                   ${PROJECT_SOURCE_DIR}/scripts/repro-check.sh ${PROJECT_SOURCE_DIR}/packaging/build-pkg.sh)
  add_test(NAME packaging.syntax_repro-check
           COMMAND ${NTFSMAC_SH} -n ${PROJECT_SOURCE_DIR}/scripts/repro-check.sh)
  # U-PKG-10 (T-M7-08): the detached signature of SHA256SUMS (scripts/sign-release.sh) with a throw-away
  # key made in a temporary directory, release-local.sh's refusal without --sign-key or --unsigned, no
  # private key in the tree, and the verify step of docs/INSTALL.md before the install command.
  find_program(NTFSMAC_SSH_KEYGEN ssh-keygen)
  if(NTFSMAC_SSH_KEYGEN)
    add_test(NAME packaging.release_signing
             COMMAND ${NTFSMAC_SH} ${CMAKE_CURRENT_SOURCE_DIR}/release_signing_test.sh ${PROJECT_SOURCE_DIR})
  endif()
  # U-PKG-11 (T-M7-17): the build guest's network-login check against a stub tart (the real guest is
  # I-REL-01): a failing or empty guest command is an error, never a pass; any non-loopback listener fails.
  add_test(NAME packaging.guest_login_check
           COMMAND ${NTFSMAC_SH} ${CMAKE_CURRENT_SOURCE_DIR}/guest_login_check_test.sh ${PROJECT_SOURCE_DIR})
endif()

# U-PKG-04 (T-M6-01): shellcheck. Every check (style level) on the scripts that run as root (the
# package scripts, uninstall.sh, dev-install.sh; audit M6-F08) and on the release build and test
# scripts; errors only on the older M4 test scripts.
find_program(NTFSMAC_SHELLCHECK shellcheck)
if(NTFSMAC_SHELLCHECK)
  add_test(NAME packaging.shellcheck_release
           COMMAND ${NTFSMAC_SHELLCHECK} -x -S style
                   packaging/scripts/preinstall packaging/scripts/postinstall packaging/build-pkg.sh
                   packaging/uninstall.sh packaging/dev-install.sh
                   packaging/payload-check.sh tests/unit/packaging/payload_lint.sh
                   packaging/bom-check.sh tests/unit/packaging/bom_check_test.sh tests/unit/packaging/root_only_test.sh
                   packaging/sign-and-notarize.sh tests/unit/packaging/sign_and_notarize_test.sh
                   scripts/make-source-release.sh scripts/lib/label.sh scripts/lib/tree.sh scripts/release-local.sh tests/unit/packaging/source_release_test.sh
                   scripts/repro-check.sh tests/unit/packaging/repro_check_test.sh
                   scripts/sign-release.sh tests/unit/packaging/release_signing_test.sh vm/macos/guest-login-check.sh
                   tests/unit/packaging/guest_login_check_test.sh
                   tests/integration/packaging/pkg_release_vm.sh tests/integration/packaging/pkg_release_vm_guest.sh
                   tests/integration/packaging/pkg_preflight_vm.sh tests/integration/packaging/pkg_preflight_vm_guest.sh
                   tests/integration/packaging/release_guest_login_vm.sh
           WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  add_test(NAME packaging.shellcheck_errors
           COMMAND ${NTFSMAC_SHELLCHECK} -x -S error
                   tests/unit/packaging/manifest_lint.sh tests/unit/packaging/manifest_lint_test.sh
                   tests/integration/packaging/pkg_vm.sh tests/integration/packaging/pkg_vm_guest.sh
           WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()
