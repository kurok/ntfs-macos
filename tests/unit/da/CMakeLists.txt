# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-02: pure parts of the Disk Arbitration module (U-DMN-DA-01..04).
# The calls that need diskarbitrationd run only in the macOS guest
# (tests/integration/daemon/da_watch.c).

add_executable(da_filter da_filter.c)
target_include_directories(da_filter PRIVATE ${PROJECT_SOURCE_DIR}/src/daemon
                           ${PROJECT_SOURCE_DIR}/tests/unit/daemon)
target_link_libraries(da_filter PRIVATE ntfsmac_daemon ntfsmac_options)
add_test(NAME daemon.da_filter COMMAND da_filter)

# Experiment tool, built on macOS only so that it does not rot. It talks to
# diskarbitrationd and is run by hand in a guest, never by ctest.
if(APPLE)
  add_executable(da_watch ${PROJECT_SOURCE_DIR}/tests/integration/daemon/da_watch.c)
  target_include_directories(da_watch PRIVATE ${PROJECT_SOURCE_DIR}/src/daemon)
  target_link_libraries(da_watch PRIVATE ntfsmac_daemon ntfsmac_options)
endif()
