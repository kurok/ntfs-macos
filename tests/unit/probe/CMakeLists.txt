# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-02: probe tests (U-PROBE-*, U-PROBE-MIRROR-*, U-PROBE-NAMES-*).
#
# Always registered (any host): probe.pure (UTF-8, state derivation, NFC and JSON writer in C) and
# probe.nfc (the NFC module against Python's unicodedata).
# Registered when NTFSMAC_PROBE_FIXTURES is set, a list of directories with fixture images made by
# tests/fixtures/gen/{unsafe,clean,names,mirror}.sh (Environment A, tests/unit/probe/run.sh):
# probe.mftmirr (the rule M1 to M5 on in-memory copies) and, when libntfs-3g is there, probe.run
# (the built probe over every fixture).

set(NTFSMAC_PROBE_FIXTURES "" CACHE STRING "Directories with fixture images for the probe tests (list)")

add_executable(test_probe_pure test_pure.c)
target_link_libraries(test_probe_pure PRIVATE ntfsmac_probe_pure ntfsmac_options)
add_test(NAME probe.pure COMMAND test_probe_pure)

add_executable(nfc_tool nfc_tool.c)
target_link_libraries(nfc_tool PRIVATE ntfsmac_probe_pure ntfsmac_options)

add_executable(mm_tool mm_tool.c)
target_link_libraries(mm_tool PRIVATE ntfsmac_mftmirr ntfsmac_options)

find_program(NTFSMAC_PYTHON3 python3)
if(NTFSMAC_PYTHON3)
  set(_names_dir "")
  foreach(_d ${NTFSMAC_PROBE_FIXTURES})
    if(EXISTS ${_d}/names-nfd.all)
      set(_names_dir --names-dir ${_d})
    endif()
  endforeach()
  add_test(NAME probe.nfc
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${NTFSMAC_PYTHON3} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_nfc.py
            --tool $<TARGET_FILE:nfc_tool> ${_names_dir})

  if(NTFSMAC_PROBE_FIXTURES)
    set(_images)
    foreach(_d ${NTFSMAC_PROBE_FIXTURES})
      list(APPEND _images --images ${_d})
    endforeach()
    # the fixture images are found by name in any of the directories
    set(_split_image "")
    set(_image64 "")
    foreach(_d ${NTFSMAC_PROBE_FIXTURES})
      if(EXISTS ${_d}/mirror-split.img)
        set(_split_image ${_d}/mirror-split.img)
      endif()
      if(EXISTS ${_d}/clean-64m-64k.img)
        set(_image64 ${_d}/clean-64m-64k.img)
      endif()
    endforeach()
    add_test(NAME probe.mftmirr
      COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
              ${NTFSMAC_PYTHON3} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_mftmirr.py
              --tool $<TARGET_FILE:mm_tool> --split-image ${_split_image} --image64 ${_image64})

    if(TARGET ntfsmac-probe AND TARGET setvolflags)
      add_test(NAME probe.run
        COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
                ${NTFSMAC_PYTHON3} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_probe.py
                --probe $<TARGET_FILE:ntfsmac-probe> --probe-test $<TARGET_FILE:probe_test>
                --manifest ${PROJECT_SOURCE_DIR}/tests/fixtures/manifest.json
                --schema ${PROJECT_SOURCE_DIR}/docs/schemas/probe.schema.json
                --contract ${PROJECT_SOURCE_DIR}/tests/unit/contracts/test_probe_schema.py
                --setvolflags $<TARGET_FILE:setvolflags>
                ${_images})
    endif()
  endif()
else()
  message(STATUS "python3 not found: probe.nfc, probe.mftmirr and probe.run tests not registered")
endif()
