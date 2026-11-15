# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-12: ntfsmac-volmark tests (U-VMK-01..10, the crash table of ARCHITECTURE.md section 3.6,
# the vmk-split image for U15).
#
# Always registered (any host): volmark.args (command line and the step-3 flag decision).
# Registered when NTFSMAC_VOLMARK_FIXTURES is set, a list of directories with fixture images made
# by tests/fixtures/gen/{unsafe,clean}.sh (Environment A, tests/unit/volmark/run.sh), and
# libntfs-3g is there: volmark.run (test_volmark.py). NTFSMAC_VOLMARK_FAULT_BIN names a
# volmark_test built against the --enable-faultio libntfs-3g (patch 0004); the cut tests need it
# and fail without it.

set(NTFSMAC_VOLMARK_FIXTURES "" CACHE STRING "Directories with fixture images for the volmark tests (list)")
set(NTFSMAC_VOLMARK_FAULT_BIN "" CACHE FILEPATH "volmark_test built against the fault-layer libntfs-3g")
set(NTFSMAC_VOLMARK_OUT "" CACHE PATH "Where volmark.run writes vmk-split.img and its record-3 report")

add_executable(test_volmark_args test_args.c)
target_link_libraries(test_volmark_args PRIVATE ntfsmac_volmark_pure ntfsmac_options)
add_test(NAME volmark.args COMMAND test_volmark_args)

find_program(NTFSMAC_PYTHON3 python3)
if(NTFSMAC_PYTHON3 AND NTFSMAC_VOLMARK_FIXTURES AND TARGET ntfsmac-volmark AND TARGET ntfsmac-probe)
  set(_images)
  foreach(_d ${NTFSMAC_VOLMARK_FIXTURES})
    list(APPEND _images --images ${_d})
  endforeach()
  set(_fault)
  if(NTFSMAC_VOLMARK_FAULT_BIN)
    set(_fault --volmark-fault ${NTFSMAC_VOLMARK_FAULT_BIN})
  endif()
  set(_out)
  if(NTFSMAC_VOLMARK_OUT)
    set(_out --out ${NTFSMAC_VOLMARK_OUT})
  endif()
  add_test(NAME volmark.run
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${NTFSMAC_PYTHON3} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_volmark.py
            --volmark $<TARGET_FILE:ntfsmac-volmark> --volmark-test $<TARGET_FILE:volmark_test>
            --probe $<TARGET_FILE:ntfsmac-probe>
            --manifest ${PROJECT_SOURCE_DIR}/tests/fixtures/manifest.json
            ${_fault} ${_out} ${_images})
endif()
