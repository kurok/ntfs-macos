# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-19: disk-derived strings (U-LABEL-*, U-VERDICT-01..05, path display of
# ADR-023). test_verdict_schema.py runs the parser against
# docs/schemas/probe.schema.json and the corpus of tests/unit/contracts.

# The tests include the CHECK macros of the policy tests.
set(_check_dir ${PROJECT_SOURCE_DIR}/tests/unit/policy)

add_executable(test_label test_label.c)
target_include_directories(test_label PRIVATE ${_check_dir})
target_link_libraries(test_label PRIVATE ntfsmac_sanitize ntfsmac_worker_options ntfsmac_options)
add_test(NAME sanitize.label COMMAND test_label)

add_executable(test_display test_display.c)
target_include_directories(test_display PRIVATE ${_check_dir})
target_link_libraries(test_display PRIVATE ntfsmac_sanitize ntfsmac_options)
add_test(NAME sanitize.display COMMAND test_display)

# The probe's JSON writer (ntfsmac_probe_pure) is the producer for the round trip; it also
# brings ntfsmac_state_rank, which the severity checks of this test use.
add_executable(test_verdict test_verdict.c)
target_include_directories(test_verdict PRIVATE ${_check_dir})
target_link_libraries(test_verdict PRIVATE ntfsmac_sanitize ntfsmac_probe_pure ntfsmac_journal
                                          ntfsmac_options)
add_test(NAME sanitize.verdict COMMAND test_verdict)

add_executable(verdict_tool verdict_tool.c)
target_link_libraries(verdict_tool PRIVATE ntfsmac_sanitize ntfsmac_probe_pure ntfsmac_options)

find_program(NTFSMAC_PYTHON3 python3)
if(NTFSMAC_PYTHON3)
  add_test(NAME sanitize.verdict_schema
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${NTFSMAC_PYTHON3} -I -B ${CMAKE_CURRENT_SOURCE_DIR}/test_verdict_schema.py
            --tool $<TARGET_FILE:verdict_tool>
            --contracts-dir ${PROJECT_SOURCE_DIR}/tests/unit/contracts
            --schema ${PROJECT_SOURCE_DIR}/docs/schemas/probe.schema.json
            --header ${PROJECT_SOURCE_DIR}/include/ntfsmac/probe.h
            --architecture ${PROJECT_SOURCE_DIR}/docs/ARCHITECTURE.md)
else()
  message(STATUS "python3 not found: sanitize.verdict_schema test not registered")
endif()
