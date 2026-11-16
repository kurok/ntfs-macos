# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-05: restart reconciliation tests (U-DMN-RC-*). The decision logic is
# pure; the run test uses a fresh temporary journal directory and never the
# production path.

add_executable(test_reconcile test_reconcile.c)
target_include_directories(test_reconcile PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/journal)
target_link_libraries(test_reconcile PRIVATE ntfsmac_daemon ntfsmac_options)
add_test(NAME daemon.reconcile COMMAND test_reconcile)
