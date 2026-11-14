# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-13: session journal library (docs/ARCHITECTURE.md section 3.5).

add_library(ntfsmac_journal STATIC journal.c)
target_include_directories(ntfsmac_journal PUBLIC ${PROJECT_SOURCE_DIR}/include
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ntfsmac_journal PUBLIC ntfsmac_common PRIVATE ntfsmac_options)

# Same sources with the kill-point hook compiled in. Linked by the crash test
# only; never installed or linked into a product binary.
add_library(ntfsmac_journal_testhooks STATIC journal.c)
target_compile_definitions(ntfsmac_journal_testhooks PRIVATE NTFSMAC_JOURNAL_TESTHOOKS=1)
target_include_directories(ntfsmac_journal_testhooks PUBLIC ${PROJECT_SOURCE_DIR}/include
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ntfsmac_journal_testhooks PUBLIC ntfsmac_common PRIVATE ntfsmac_options)
