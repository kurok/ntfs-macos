# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M5-09: F-CTL-01, libFuzzer on the control socket parser and handler (docs/TEST_STRATEGY.md
# section 8). Always built: a replay executable (the F-SANITIZE-* driver) that runs the seed corpus
# and is registered with ctest. With -DNTFSMAC_FUZZ=ON (clang with libFuzzer, Environment A,
# tests/fuzz/campaign/run.sh) the libFuzzer target fuzz_ctl is added. The code under test (the
# whole ntfsmac_cli library and what it links) is instrumented by the build's own flags
# (tests/fuzz/campaign/inner.sh passes -fsanitize=fuzzer-no-link,address,undefined in CMAKE_C_FLAGS).

# The option NTFSMAC_FUZZ is declared in tests/fuzz/sanitize/CMakeLists.txt (added before this directory).

set(_corpus ${CMAKE_CURRENT_SOURCE_DIR}/corpus)
add_executable(fuzz_replay_ctl ../sanitize/fuzz_main.c fuzz_ctl.c)
target_include_directories(fuzz_replay_ctl PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/ctl)
target_link_libraries(fuzz_replay_ctl PRIVATE ntfsmac_cli ntfsmac_options)
add_test(NAME fuzz.ctl_seeds COMMAND fuzz_replay_ctl ${_corpus})

if(NTFSMAC_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "NTFSMAC_FUZZ needs clang (libFuzzer)")
  endif()
  add_executable(fuzz_ctl fuzz_ctl.c)
  target_include_directories(fuzz_ctl PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/ctl)
  target_compile_options(fuzz_ctl PRIVATE -g -O1 -fsanitize=fuzzer-no-link,address,undefined
                                          -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_libraries(fuzz_ctl PRIVATE ntfsmac_cli ntfsmac_options)
  target_link_options(fuzz_ctl PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# Synthetic-log tests of the campaign tooling (tests/fuzz/campaign/test_campaign.py).
find_program(NTFSMAC_PYTHON3 python3)
if(NTFSMAC_PYTHON3)
  add_test(NAME fuzz.campaign_tools
    COMMAND ${NTFSMAC_PYTHON3} -I -B ${PROJECT_SOURCE_DIR}/tests/fuzz/campaign/test_campaign.py)
else()
  message(STATUS "python3 not found: fuzz.campaign_tools not registered")
endif()
