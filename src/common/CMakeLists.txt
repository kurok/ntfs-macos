# SPDX-License-Identifier: GPL-2.0-or-later

add_library(ntfsmac_common STATIC log.c)
target_include_directories(ntfsmac_common PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ntfsmac_common PRIVATE ntfsmac_options)

add_executable(test_log test_log.c)
target_link_libraries(test_log PRIVATE ntfsmac_common ntfsmac_options)
add_test(NAME common.log COMMAND test_log)
