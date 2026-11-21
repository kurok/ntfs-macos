# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests of the checkers that every milestone gate depends on (audit M5-F08): the CI runner and its
# --verify-result rules, the gate case list, and the selftests of the stress and performance result
# builders (none needs a VM, a mount or root). They run in `ctest` on the host and in the Linux
# container stage (linux-build-test). Exit status 77 is a skip (SKIP_RETURN_CODE), printed with its reason:
#   - the two CI shell tests need a git binary (the container has one, and mounts the whole repository,
#     .git included, at /work); ci.local_full only runs git in a throw-away repository of its own, ci.local
#     only through ci-local.sh, which passes `-c safe.directory=*` (no "dubious ownership" for root on a
#     uid-501 mount); the real-build part of test_ci_local.sh (ci.local_build) also needs the .git of the source;
#   - ci.local_build runs ci-local.sh --quick inside real copies, whose ctest would run these tests again:
#     NTFSMAC_CI_NESTED=1 makes them skip there.
find_program(NTFSMAC_PYTHON3 python3)
find_program(NTFSMAC_SH sh)
set(_ci_dir ${CMAKE_CURRENT_SOURCE_DIR})
set(_ci_env ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1)

if(NTFSMAC_PYTHON3 AND NTFSMAC_SH)
  # --usage: the option and exit-code checks and the quick-mode stage propagation with fake cmake and ctest
  # (a few seconds, no build); the three real build-and-ctest runs of the
  # copies take two minutes: see ci.local_build below.
  add_test(NAME ci.local
    COMMAND ${_ci_env} ${NTFSMAC_SH} ${_ci_dir}/test_ci_local.sh --usage)
  add_test(NAME ci.local_full
    COMMAND ${_ci_env} ${NTFSMAC_SH} ${_ci_dir}/test_ci_local_full.sh)
  # the clang-tidy lookup of the static-analysis stage, on a fake PATH (seconds)
  add_test(NAME ci.local_tools
    COMMAND ${_ci_env} ${NTFSMAC_SH} ${_ci_dir}/test_ci_local_tools.sh)
  set_tests_properties(ci.local_tools PROPERTIES TIMEOUT 60 LABELS ci)
  add_test(NAME ci.gate_cases
    COMMAND ${NTFSMAC_PYTHON3} -I -B ${_ci_dir}/gate_cases_check.py ${PROJECT_SOURCE_DIR} M5)
  set_tests_properties(ci.local ci.local_full PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 LABELS ci)
  set_tests_properties(ci.gate_cases PROPERTIES TIMEOUT 60 LABELS ci)

  option(NTFSMAC_CI_BUILD_TESTS "Also register ci.local_build: ci-local.sh --quick on copies (about 2 minutes)" OFF)
  if(NTFSMAC_CI_BUILD_TESTS)
    add_test(NAME ci.local_build
      COMMAND ${_ci_env} ${NTFSMAC_SH} ${_ci_dir}/test_ci_local.sh)
    set_tests_properties(ci.local_build PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 LABELS ci RUN_SERIAL TRUE)
  endif()

  # selftests of the result builders and classifiers (they also run at the start of each VM run)
  set(_selftests
    stress.soak_mixlib=tests/stress/soak/mixlib.py
    stress.soak_result=tests/stress/soak/mix_result.py
    stress.kill_result=tests/stress/kill/result.py
    stress.disconnect_result=tests/stress/disconnect/disc_result.py
    stress.leaks_classify=tests/stress/leaks/classify.py
    perf.result=tests/perf/result.py
    perf.report=tests/perf/report.py)
  foreach(_t IN LISTS _selftests)
    string(REPLACE "=" ";" _pair "${_t}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _script)
    if(_name STREQUAL "stress.soak_mixlib")
      # its selftest starts workers and looks them up with pgrep (procps; the Linux image may lack it). The module
      # runs in the macOS guest only: its torn-read check of the shared slots (one aligned 8-byte store per field)
      # holds on macOS and was measured to fail on Linux in the container (229 torn reads of 1.6 million), so it
      # is skipped, visibly, off macOS.
      add_test(NAME ${_name}
        COMMAND ${NTFSMAC_SH} -c "[ \"$(uname -s)\" = Darwin ] || { echo 'SKIP: mixlib runs in the macOS guest only (slot store atomicity is not given on this OS)'; exit 77; }; command -v pgrep >/dev/null 2>&1 || { echo 'SKIP: pgrep not installed'; exit 77; }; exec \"$0\" -I -B \"$1\" selftest"
                ${NTFSMAC_PYTHON3} ${PROJECT_SOURCE_DIR}/${_script})
      set_tests_properties(${_name} PROPERTIES SKIP_RETURN_CODE 77)
    else()
      add_test(NAME ${_name}
        COMMAND ${NTFSMAC_PYTHON3} -I -B ${PROJECT_SOURCE_DIR}/${_script} selftest)
    endif()
    set_tests_properties(${_name} PROPERTIES TIMEOUT 120 LABELS ci)
    # timing-sensitive asserts (6 s workload, process lookups, throughput numbers)
    if(_name STREQUAL "stress.soak_mixlib" OR _name STREQUAL "perf.result")
      set_tests_properties(${_name} PROPERTIES RUN_SERIAL TRUE)
    endif()
  endforeach()
else()
  message(STATUS "python3 or sh not found: the ci.* and selftest tests are not registered")
endif()
