# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-04: control socket and CLI tests (U-CTL-*). Host tests: fake system
# facts, no root, no signing, no authorization. The VM probe (ctl_vm_probe.c)
# is not a ctest: it needs root, the installed right and a signed client, and
# runs in a disposable macOS VM only (see its header). Tests link ntfsmac_cli,
# which brings the daemon library with it.

foreach(t parse peer handle fuzz socket cli xpc)
  add_executable(ctl_${t} ctl_${t}.c)
  target_include_directories(ctl_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ctl_${t} PRIVATE ntfsmac_cli ntfsmac_options)
  add_test(NAME ctl.${t} COMMAND ctl_${t})
endforeach()
set_tests_properties(ctl.socket PROPERTIES TIMEOUT 60)
set_tests_properties(ctl.fuzz PROPERTIES TIMEOUT 120)

# VM-only probe (not a test): see ctl_vm_probe.c and vm/u17.sh.
if(APPLE)
  add_executable(ctl_vm_probe ctl_vm_probe.c)
  target_include_directories(ctl_vm_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ctl_vm_probe PRIVATE ntfsmac_cli ntfsmac_options)
endif()
