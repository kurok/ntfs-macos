# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-00: daemon library. Every .c file in this directory except main.c is
# collected by the glob, so a later task adds a source file without editing
# this file (docs/AGENT_TASKS.md, ownership table). CONFIGURE_DEPENDS makes
# the build re-run CMake when a file is added or removed.
#
# main.c belongs to T-M4-08. Until it exists there is no executable. When it
# is added, it needs Disk Arbitration and CoreFoundation, which T-M4-08 wires
# in (the section 3 table of docs/ARCHITECTURE.md lists them).

file(GLOB NTFSMAC_DAEMON_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
list(FILTER NTFSMAC_DAEMON_SOURCES EXCLUDE REGEX "/main\\.c$")

find_package(Threads REQUIRED) # spawn.c (T-M4-01) uses a mutex around fork

add_library(ntfsmac_daemon STATIC ${NTFSMAC_DAEMON_SOURCES})
target_include_directories(ntfsmac_daemon PUBLIC ${PROJECT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ntfsmac_daemon PUBLIC
  ntfsmac_policy ntfsmac_journal ntfsmac_sanitize ntfsmac_worker_options Threads::Threads)
target_link_libraries(ntfsmac_daemon PRIVATE ntfsmac_options)

# da.c (T-M4-02) is the only file here that uses Disk Arbitration.
if(APPLE)
  target_link_libraries(ntfsmac_daemon PUBLIC
    "-framework DiskArbitration" "-framework CoreFoundation")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/main.c)
  add_executable(ntfsmacd main.c)
  target_link_libraries(ntfsmacd PRIVATE ntfsmac_daemon ntfsmac_options)
endif()
