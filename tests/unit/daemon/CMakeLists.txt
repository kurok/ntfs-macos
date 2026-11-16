# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-00: daemon state machine and stub module tests (U-DMN-SM-*).

foreach(t transitions invalid stubs)
  add_executable(statemachine_${t} statemachine_${t}.c)
  target_include_directories(statemachine_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(statemachine_${t} PRIVATE ntfsmac_daemon ntfsmac_options)
  add_test(NAME daemon.statemachine_${t} COMMAND statemachine_${t})
endforeach()
