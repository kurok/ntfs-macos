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
foreach(t mountpoint unmount errwatch)
  add_executable(${t}_test ${t}_test.c)
  target_include_directories(${t}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${t}_test PRIVATE ntfsmac_daemon ntfsmac_options)
  add_test(NAME daemon.${t} COMMAND ${t}_test)
endforeach()
