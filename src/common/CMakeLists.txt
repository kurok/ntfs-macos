# SPDX-License-Identifier: GPL-2.0-or-later

add_library(ntfsmac_common STATIC log.c)
target_include_directories(ntfsmac_common PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ntfsmac_common PRIVATE ntfsmac_options)

add_executable(test_log test_log.c)
target_link_libraries(test_log PRIVATE ntfsmac_common ntfsmac_options)
add_test(NAME common.log COMMAND test_log)

# T-M3-02: the strict stale-$MFTMirr rule (ARCHITECTURE.md section 3.7), shared by
# the probe and volmark (T-M4-16). Pure C, no libntfs-3g.
add_library(ntfsmac_mftmirr STATIC mftmirr.c)
target_include_directories(ntfsmac_mftmirr PUBLIC ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ntfsmac_mftmirr PRIVATE ntfsmac_options)
