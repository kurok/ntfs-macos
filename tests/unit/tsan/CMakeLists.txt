# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-15: concurrent drivers for the daemon logic (U-TSAN-01..02). They run
# in every build as plain concurrency tests. Under the tsan preset
# (cmake --preset tsan) ThreadSanitizer checks them for data races.

foreach(t statemachine ctl_journal)
  add_executable(tsan_${t} tsan_${t}.c)
  target_include_directories(tsan_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(tsan_${t} PRIVATE ntfsmac_daemon ntfsmac_options)
  add_test(NAME tsan.${t} COMMAND tsan_${t})
endforeach()
set_tests_properties(tsan.statemachine PROPERTIES TIMEOUT 120)
set_tests_properties(tsan.ctl_journal PROPERTIES TIMEOUT 240)
