# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-10: host-side checks of the install manifest (packaging/manifest.txt). They read files only.
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
  # The scripts at least parse (they are run only in the guest).
  foreach(s dev-install uninstall build-pkg)
    add_test(NAME packaging.syntax_${s}
             COMMAND ${NTFSMAC_SH} -n ${PROJECT_SOURCE_DIR}/packaging/${s}.sh)
  endforeach()
endif()
