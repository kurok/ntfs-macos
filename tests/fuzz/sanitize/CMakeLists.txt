# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-19: F-SANITIZE-01..03 (docs/TEST_STRATEGY.md section 8).
#
# Always built: replay executables (fuzz_main.c as driver) that run the seed corpus through
# the same harness functions and are registered with ctest. Option -DNTFSMAC_FUZZ=ON (clang
# with libFuzzer; Environment A, tests/fuzz/sanitize/run.sh) adds the libFuzzer targets
# fuzz_sanitize_{verdict,label,display}. The code under test is compiled again for them with
# -fsanitize=fuzzer-no-link,address,undefined so that coverage guides the mutation.

option(NTFSMAC_FUZZ "Build the libFuzzer targets of the F-SANITIZE-* tests (clang)" OFF)

set(_corpus ${CMAKE_CURRENT_SOURCE_DIR}/corpus)
set(_targets verdict label display)

# Sources of the code the harnesses exercise (the harnesses are the only user).
set(_srcs
  ${PROJECT_SOURCE_DIR}/src/common/label_cp.c
  ${PROJECT_SOURCE_DIR}/src/sanitize/label.c
  ${PROJECT_SOURCE_DIR}/src/sanitize/display.c
  ${PROJECT_SOURCE_DIR}/src/sanitize/verdict.c
  ${PROJECT_SOURCE_DIR}/src/options/options.c
  ${PROJECT_SOURCE_DIR}/src/options/argv_check.c
  ${PROJECT_SOURCE_DIR}/src/probe/json.c)

foreach(_t ${_targets})
  add_executable(fuzz_replay_${_t} fuzz_main.c fuzz_${_t}.c)
  target_include_directories(fuzz_replay_${_t} PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/sanitize ${PROJECT_SOURCE_DIR}/src/probe)
  # The harness aborts on an oracle failure, so a failing replay is a failing test.
  target_link_libraries(fuzz_replay_${_t} PRIVATE ntfsmac_sanitize ntfsmac_worker_options
                                                  ntfsmac_probe_pure ntfsmac_options)
  add_test(NAME fuzz.sanitize_${_t}_seeds COMMAND fuzz_replay_${_t} ${_corpus}/${_t})
endforeach()

# Regression input of finding M5-01 (docs/SECURITY_FINDINGS.md): argv_check rule 2 accepted a leaf that
# starts with "-". The replay aborts on the oracle failure unpatched. Same naming as fuzz.findings.F1/F3/F4/F8/F9.
add_test(NAME fuzz.findings.M5-01
  COMMAND fuzz_replay_label ${PROJECT_SOURCE_DIR}/tests/fuzz/findings/M5-01-label.bin)

if(NTFSMAC_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "NTFSMAC_FUZZ needs clang (libFuzzer)")
  endif()
  add_library(fuzz_sanitize_code OBJECT ${_srcs})
  target_include_directories(fuzz_sanitize_code PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/sanitize ${PROJECT_SOURCE_DIR}/src/probe)
  target_compile_options(fuzz_sanitize_code PRIVATE
    -g -O1 -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all
    -fno-omit-frame-pointer)
  foreach(_t ${_targets})
    add_executable(fuzz_sanitize_${_t} fuzz_${_t}.c $<TARGET_OBJECTS:fuzz_sanitize_code>)
    target_include_directories(fuzz_sanitize_${_t} PRIVATE
      ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/sanitize ${PROJECT_SOURCE_DIR}/src/probe)
    target_compile_options(fuzz_sanitize_${_t} PRIVATE
      -g -O1 -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all
      -fno-omit-frame-pointer)
    target_link_options(fuzz_sanitize_${_t} PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
endif()
