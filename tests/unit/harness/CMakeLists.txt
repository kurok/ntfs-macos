# SPDX-License-Identifier: GPL-2.0-or-later
#
# Unit tests of the Python test harness (T-M2-08). Skipped when python3 is absent.
find_program(NTFSMAC_PYTHON3 python3)
if(NTFSMAC_PYTHON3)
  add_test(NAME harness.unit
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${NTFSMAC_PYTHON3} -B -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR})
else()
  message(STATUS "python3 not found: harness.unit test not registered")
endif()
