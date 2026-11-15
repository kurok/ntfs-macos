# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-16: F-PROBE and F-MOUNT-WALK (docs/TEST_STRATEGY.md section 8).
#
# Needs libntfs-3g (Environment A, tests/fuzz/image/run.sh; found through PKG_CONFIG_PATH like
# the probe, which is skipped without it: so is this directory).
#
# Always built when the library is there: replay executables (the driver of the F-SANITIZE-*
# tests as main) that run the seed corpus through the harness functions, registered with ctest
# as fuzz.probe_seeds and fuzz.mountwalk_seeds. Option -DNTFSMAC_FUZZ=ON (clang with libFuzzer)
# adds the libFuzzer targets fuzz_probe and fuzz_mountwalk: the probe sources are compiled again
# with -fsanitize=fuzzer-no-link,address,undefined so that coverage guides the mutation.
# libntfs-3g is then expected to be built with the same instrumentation (inner.sh does it).

find_package(PkgConfig QUIET)
if(NOT PkgConfig_FOUND)
  message(STATUS "pkg-config not found: skipping the image fuzz harnesses")
  return()
endif()
pkg_check_modules(NTFS3G_FZ QUIET IMPORTED_TARGET libntfs-3g)
if(NOT NTFS3G_FZ_FOUND OR NOT TARGET ntfsmac_probe_core)
  message(STATUS "libntfs-3g not found: skipping the image fuzz harnesses")
  return()
endif()

option(NTFSMAC_FUZZ "Build the libFuzzer targets of the fuzz tests (clang)" OFF)

set(_bases ${CMAKE_CURRENT_SOURCE_DIR}/bases)
set(_corpus ${CMAKE_CURRENT_SOURCE_DIR}/corpus)
set(_driver ${CMAKE_CURRENT_SOURCE_DIR}/../sanitize/fuzz_main.c)
set(_incs ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/probe ${PROJECT_SOURCE_DIR}/src/sanitize
          ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(fuzz_replay_probe ${_driver} fuzz_probe.c image.c)
target_include_directories(fuzz_replay_probe PRIVATE ${_incs})
target_compile_definitions(fuzz_replay_probe PRIVATE FZ_BASES_DIR="${_bases}")
target_link_libraries(fuzz_replay_probe PRIVATE ntfsmac_probe_core ntfsmac_sanitize ntfsmac_options)
add_test(NAME fuzz.probe_seeds COMMAND fuzz_replay_probe ${_corpus}/probe)

add_executable(fuzz_replay_mountwalk ${_driver} fuzz_mountwalk.c image.c)
target_include_directories(fuzz_replay_mountwalk PRIVATE ${_incs})
target_compile_definitions(fuzz_replay_mountwalk PRIVATE FZ_BASES_DIR="${_bases}")
target_link_libraries(fuzz_replay_mountwalk PRIVATE PkgConfig::NTFS3G_FZ ntfsmac_options)
add_test(NAME fuzz.mountwalk_seeds COMMAND fuzz_replay_mountwalk ${_corpus}/mountwalk)

# Some seeds end in a failed mount, and libntfs-3g leaks the run list of the $MFT on that path
# (tests/fuzz/TRIAGE.md, finding F2). Under a leak-checking sanitizer build the replay must not
# fail for that; run.sh checks the leaks of the whole corpus in its own stage and lists them.
set_tests_properties(fuzz.probe_seeds fuzz.mountwalk_seeds PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

if(NTFSMAC_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "NTFSMAC_FUZZ needs clang (libFuzzer)")
  endif()
  set(_fuzz_flags -g -O1 -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all
                  -fno-omit-frame-pointer)

  # The code under test, compiled once for the F-PROBE target (the probe core, its pure parts,
  # the stale-mirror rule and the daemon's verdict parser, which the oracle uses).
  add_library(fuzz_probe_code OBJECT
    ${PROJECT_SOURCE_DIR}/src/probe/probe.c
    ${PROJECT_SOURCE_DIR}/src/probe/devio.c
    ${PROJECT_SOURCE_DIR}/src/probe/names.c
    ${PROJECT_SOURCE_DIR}/src/probe/nfc.c
    ${PROJECT_SOURCE_DIR}/src/probe/state.c
    ${PROJECT_SOURCE_DIR}/src/probe/text.c
    ${PROJECT_SOURCE_DIR}/src/probe/json.c
    ${PROJECT_SOURCE_DIR}/src/common/mftmirr.c
    ${PROJECT_SOURCE_DIR}/src/common/state_rank.c
    ${PROJECT_SOURCE_DIR}/src/common/label_cp.c
    ${PROJECT_SOURCE_DIR}/src/sanitize/verdict.c
    ${PROJECT_SOURCE_DIR}/src/sanitize/label.c
    ${PROJECT_SOURCE_DIR}/src/sanitize/display.c)
  target_include_directories(fuzz_probe_code PRIVATE ${_incs} ${PROJECT_SOURCE_DIR}/src/common)
  target_compile_options(fuzz_probe_code PRIVATE ${_fuzz_flags})
  target_link_libraries(fuzz_probe_code PRIVATE PkgConfig::NTFS3G_FZ)

  add_executable(fuzz_probe fuzz_probe.c image.c $<TARGET_OBJECTS:fuzz_probe_code>)
  target_include_directories(fuzz_probe PRIVATE ${_incs} ${PROJECT_SOURCE_DIR}/src/common)
  target_compile_definitions(fuzz_probe PRIVATE FZ_BASES_DIR="${_bases}")
  target_compile_options(fuzz_probe PRIVATE ${_fuzz_flags})
  target_link_libraries(fuzz_probe PRIVATE PkgConfig::NTFS3G_FZ)
  target_link_options(fuzz_probe PRIVATE -fsanitize=fuzzer,address,undefined)

  add_executable(fuzz_mountwalk fuzz_mountwalk.c image.c)
  target_include_directories(fuzz_mountwalk PRIVATE ${_incs})
  target_compile_definitions(fuzz_mountwalk PRIVATE FZ_BASES_DIR="${_bases}")
  target_compile_options(fuzz_mountwalk PRIVATE ${_fuzz_flags})
  target_link_libraries(fuzz_mountwalk PRIVATE PkgConfig::NTFS3G_FZ)
  target_link_options(fuzz_mountwalk PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
