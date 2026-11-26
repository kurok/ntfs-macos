# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-00: daemon state machine and stub module tests (U-DMN-SM-*).

foreach(t transitions invalid stubs)
  add_executable(statemachine_${t} statemachine_${t}.c)
  target_include_directories(statemachine_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(statemachine_${t} PRIVATE ntfsmac_daemon ntfsmac_options)
  add_test(NAME daemon.statemachine_${t} COMMAND statemachine_${t})
endforeach()

# T-M4-03: mount point and unmount modules. T-M4-13: session channel (errwatch).
foreach(t mountpoint unmount unmount_ev unmount_verify errwatch session privops mountwatch notify startup qbounded volreuse)
  add_executable(${t}_test ${t}_test.c)
  target_include_directories(${t}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${t}_test PRIVATE ntfsmac_daemon ntfsmac_options)
  add_test(NAME daemon.${t} COMMAND ${t}_test)
endforeach()

# T-M4-08: replies of list, status and doctor, parsed back with the CLI's reader.
add_executable(report_test report_test.c)
target_include_directories(report_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(report_test PRIVATE ntfsmac_cli ntfsmac_daemon ntfsmac_options)
add_test(NAME daemon.report COMMAND report_test)

# T-M4-08: the launchd job names the privileged XPC service.
add_executable(plist_test plist_test.c)
target_include_directories(plist_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(plist_test PRIVATE ntfsmac_daemon ntfsmac_options)
target_compile_definitions(plist_test PRIVATE
  NTFSMAC_PLIST_PATH="${PROJECT_SOURCE_DIR}/integration/launchd/org.ntfs-macos.ntfsmacd.plist")
add_test(NAME daemon.plist COMMAND plist_test)
