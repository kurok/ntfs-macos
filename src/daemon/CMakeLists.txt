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

# T-M4-06: test seams of the spawn module (spawn_config.h): another sandbox directory or tool,
# no sandbox at all, a hook after argv_check(). They are on for the build that runs the tests and
# off for the build that ships (preset `product`), so a shipped daemon cannot start a child
# without the sandbox. The definition is PUBLIC: the tests include spawn_config.h and must see the
# same struct as the library.
option(NTFSMAC_TEST_SEAMS "Compile the test seams of the spawn module in" ON)
if(NTFSMAC_TEST_SEAMS)
  target_compile_definitions(ntfsmac_daemon PUBLIC NTFSMAC_TEST_SEAMS)
endif()

# da.c (T-M4-02) is the only file here that uses Disk Arbitration.
# T-M4-04: ctl_sys.c uses code signing and Authorization Services (Security).
if(APPLE)
  target_link_libraries(ntfsmac_daemon PUBLIC
    "-framework DiskArbitration" "-framework Security" "-framework CoreFoundation")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/main.c)
  add_executable(ntfsmacd main.c)
  target_link_libraries(ntfsmacd PRIVATE ntfsmac_daemon ntfsmac_options)
endif()
