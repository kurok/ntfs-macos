# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-10, T-M6-01: host-side checks of the install manifest (packaging/manifest.txt). They read files only.
# The installer and the uninstaller run in a disposable macOS guest (I-PKG-01..02,
# tests/integration/packaging/pkg_vm.sh), never on the host.

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
  # The scripts at least parse (they are run only in the guest).
  foreach(s dev-install uninstall build-pkg payload-check)
    add_test(NAME packaging.syntax_${s}
             COMMAND ${NTFSMAC_SH} -n ${PROJECT_SOURCE_DIR}/packaging/${s}.sh)
  endforeach()
  foreach(s preinstall postinstall)
    add_test(NAME packaging.syntax_${s}
             COMMAND ${NTFSMAC_SH} -n ${PROJECT_SOURCE_DIR}/packaging/scripts/${s})
  endforeach()
endif()

# U-PKG-04 (T-M6-01): shellcheck. Every check (style level) on the scripts that run as root from the
# package and on the release build and test scripts; errors only on the older packaging scripts.
find_program(NTFSMAC_SHELLCHECK shellcheck)
if(NTFSMAC_SHELLCHECK)
  add_test(NAME packaging.shellcheck_release
           COMMAND ${NTFSMAC_SHELLCHECK} -x -S style
                   packaging/scripts/preinstall packaging/scripts/postinstall packaging/build-pkg.sh
                   packaging/payload-check.sh tests/unit/packaging/payload_lint.sh
                   tests/integration/packaging/pkg_release_vm.sh tests/integration/packaging/pkg_release_vm_guest.sh
                   tests/integration/packaging/pkg_preflight_vm.sh tests/integration/packaging/pkg_preflight_vm_guest.sh
           WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  add_test(NAME packaging.shellcheck_errors
           COMMAND ${NTFSMAC_SHELLCHECK} -x -S error
                   packaging/dev-install.sh packaging/uninstall.sh
                   tests/unit/packaging/manifest_lint.sh tests/unit/packaging/manifest_lint_test.sh
                   tests/integration/packaging/pkg_vm.sh tests/integration/packaging/pkg_vm_guest.sh
           WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()
