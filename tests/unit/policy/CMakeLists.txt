# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-03: policy library and worker option builder tests (U-POL-*, U-ARGV-*).

add_executable(test_policy test_policy.c)
target_link_libraries(test_policy PRIVATE ntfsmac_policy ntfsmac_options)
add_test(NAME policy.table COMMAND test_policy)

add_executable(test_options test_options.c)
target_link_libraries(test_options PRIVATE ntfsmac_worker_options ntfsmac_policy ntfsmac_options)
add_test(NAME policy.options COMMAND test_options)

add_executable(argv_tool argv_tool.c)
target_link_libraries(argv_tool PRIVATE ntfsmac_worker_options ntfsmac_options)

# U-ARGV-06 against integration/mount-options.json (Python standard library only).
find_program(NTFSMAC_PYTHON3 python3)
if(NTFSMAC_PYTHON3)
  add_test(NAME policy.mount_options_json
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${NTFSMAC_PYTHON3} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_mount_options_json.py
            --tool $<TARGET_FILE:argv_tool>
            --json ${PROJECT_SOURCE_DIR}/integration/mount-options.json)
else()
  message(STATUS "python3 not found: policy.mount_options_json test not registered")
endif()
