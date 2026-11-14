# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-00: contract headers compile test and probe schema check.

# Each header must be self-contained: one translation unit per header that
# includes nothing else.
set(_hdr_tus)
foreach(_h probe policy journal options)
  set(_tu ${CMAKE_CURRENT_BINARY_DIR}/hdr_${_h}.c)
  file(WRITE ${_tu}
    "/* SPDX-License-Identifier: GPL-2.0-or-later */\n"
    "#include \"ntfsmac/${_h}.h\"\n"
    "extern int ntfsmac_hdr_${_h}_ok;\n"
    "int ntfsmac_hdr_${_h}_ok = 1;\n")
  list(APPEND _hdr_tus ${_tu})
endforeach()

add_executable(test_headers test_headers.c ${_hdr_tus})
target_include_directories(test_headers PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_headers PRIVATE ntfsmac_options)
add_test(NAME contracts.headers COMMAND test_headers)

# Schema check (Python stdlib only). Skipped when python3 is absent.
find_program(NTFSMAC_PYTHON3 python3)
if(NTFSMAC_PYTHON3)
  add_test(NAME contracts.probe_schema
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${NTFSMAC_PYTHON3} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_probe_schema.py
            --schema ${PROJECT_SOURCE_DIR}/docs/schemas/probe.schema.json
            --header ${PROJECT_SOURCE_DIR}/include/ntfsmac/probe.h
            --architecture ${PROJECT_SOURCE_DIR}/docs/ARCHITECTURE.md)
else()
  message(STATUS "python3 not found: contracts.probe_schema test not registered")
endif()
