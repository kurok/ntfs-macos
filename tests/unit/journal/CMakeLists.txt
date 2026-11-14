# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-13: session journal tests (U-JRN-*). Every test uses a fresh temporary
# directory; the production path /var/db/ntfs-macos/journal is never touched.

add_executable(test_journal test_journal.c)
target_link_libraries(test_journal PRIVATE ntfsmac_journal ntfsmac_options)
add_test(NAME journal.unit COMMAND test_journal)

# Crash test: fork, SIGKILL the child at every kill point of an operation.
# Links the variant of the library with the kill-point hook compiled in.
add_executable(test_journal_crash test_journal_crash.c)
target_link_libraries(test_journal_crash PRIVATE ntfsmac_journal_testhooks ntfsmac_options)
add_test(NAME journal.crash COMMAND test_journal_crash)
