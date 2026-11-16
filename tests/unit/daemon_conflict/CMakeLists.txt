# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-07: conflict detection, passive mode and doctor checks
# (U-DMN-CFL-*, U-DMN-DOC-*). Every test uses a fresh temporary directory;
# /Library/Filesystems and the production journal are never touched.

foreach(t conflict doctor)
  add_executable(test_daemon_${t} test_${t}.c)
  target_include_directories(test_daemon_${t} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/unit/daemon)
  target_link_libraries(test_daemon_${t} PRIVATE ntfsmac_daemon ntfsmac_options)
  add_test(NAME daemon.${t} COMMAND test_daemon_${t})
endforeach()
