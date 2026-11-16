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

# Regression replay of the NTFS-3G findings F1, F3 and F4 (tests/fuzz/TRIAGE.md; fixed locally by the
# patches 0012, 0010 and 0011, ADR-026), one ctest per finding so that one abort does not hide the
# others: fuzz.findings.F1, fuzz.findings.F3, fuzz.findings.F4. F2 is a known leak and stays out.
# The security descriptor reads are not skipped (NTFSMAC_FUZZ_SKIP_SECURITY is removed).
#
# They are registered only for an AddressSanitizer build (NTFSMAC_SANITIZE contains "address", and
# libntfs-3g built with -fsanitize=address as well): a build without it cannot detect the over-reads
# or the allocation, so the tests would pass unpatched. Checked on the unpatched library (T-M3-25),
# libntfs-3g and the replay built with the same sanitizer flags:
#   F1 and F4 fail unpatched in every ASan build tried (clang and gcc, -O0 and -O1, with and without UBSan).
#   F3 fails unpatched in clang -O0/-O1, gcc -O0, and gcc -O1 with "address,undefined"; it PASSES
#   unpatched in gcc -O1 with "address" alone: the loads at acls.c:682-685 are used only after the
#   length test, and that build removes them, so no over-read happens. This is the build of the
#   asan stage of tests/unit/probe/inner.sh (gcc -O1, -fsanitize=address). There fuzz.findings.F3
#   cannot show the defect; it only shows that the patched library does not crash. F3 is then
#   checked by valgrind on the standalone image or by an -O0 or clang build (docs/AGENT_TASKS.md
#   T-M3-25). With the patches all three pass in every build above.
if(NTFSMAC_SANITIZE MATCHES "address")
  set(_findings ${CMAKE_CURRENT_SOURCE_DIR}/findings)
  # The image sets ASAN_OPTIONS (log_path and more); the options of the replay are appended to them.
  set(_asan_extra "detect_leaks=0:allocator_may_return_null=0:max_allocation_size_mb=1024")
  if(DEFINED ENV{ASAN_OPTIONS} AND NOT "$ENV{ASAN_OPTIONS}" STREQUAL "")
    set(_asan_opts "$ENV{ASAN_OPTIONS}:${_asan_extra}")
  else()
    set(_asan_opts "${_asan_extra}")
  endif()
  foreach(_f IN ITEMS F1-getsecurityattr-alloc F3-valid-descr-heap-overflow F4-possible-symlink-heap-overflow)
    string(REGEX MATCH "^F[0-9]+" _id ${_f})
    add_test(NAME fuzz.findings.${_id}
      COMMAND ${CMAKE_COMMAND} -E env --unset=NTFSMAC_FUZZ_SKIP_SECURITY $<TARGET_FILE:fuzz_replay_mountwalk>
              ${_findings}/${_f}.bin)
    set_tests_properties(fuzz.findings.${_id} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${_asan_opts}")
  endforeach()
else()
  message(STATUS "no AddressSanitizer build: fuzz.findings.F1/F3/F4 not registered")
endif()

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
