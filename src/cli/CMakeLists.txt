# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-04: the `ntfsmac` command line tool. The library holds everything but
# main(), so tests can call it. T-M4-14 filled cmd_journal.c and cmd_repair.c and added priv.c and xpc_client.c.
#
# The wire names (ntfsmac_ctl_*_name) are defined in the daemon library's
# ctl_parse.c, so the CLI links ntfsmac_daemon; the linker pulls only the
# objects it needs.

add_library(ntfsmac_cli STATIC
  authz.c
  cli.c
  client.c
  cmd_journal.c
  cmd_repair.c
  json.c
  priv.c
  reasons.c
  render.c
  req.c
  xpc_client.c)
target_include_directories(ntfsmac_cli PUBLIC ${PROJECT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ntfsmac_cli PUBLIC ntfsmac_daemon PRIVATE ntfsmac_options)
if(APPLE)
  target_link_libraries(ntfsmac_cli PUBLIC "-framework Security" "-framework CoreFoundation")
endif()

add_executable(ntfsmac main.c)
target_link_libraries(ntfsmac PRIVATE ntfsmac_cli ntfsmac_options)
