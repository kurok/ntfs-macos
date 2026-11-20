# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-01: tests of the spawn module (U-SPN-*). spawn_stub is a stand-in for the
# probe, volmark and the worker; its behaviour is chosen by the base name of
# the path it is started under (the test makes one symlink per behaviour).

# Issue 1 (audit M4 F-02): which holder of the device the sweep may kill, with a fake holder list
# (U-HLD-*). Pure code, so no test seams are needed.
add_executable(test_holder test_holder.c)
target_include_directories(test_holder PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/daemon)
target_link_libraries(test_holder PRIVATE ntfsmac_daemon ntfsmac_options)
add_test(NAME daemon.spawn_holder COMMAND test_holder ${PROJECT_SOURCE_DIR}/integration/sandbox)

# Issue 2 (audit M4 F-03, ADR-012): the grammar of the usermapping file and the decision whether
# `usermapping=` may be enabled (U-UMP-01..06, pure), and the daemon's reading and re-check of the
# file on real files in a scratch directory (U-UMP-10..17). Neither needs the test seams.
add_executable(test_usermap test_usermap.c)
target_include_directories(test_usermap PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/daemon)
target_link_libraries(test_usermap PRIVATE ntfsmac_daemon ntfsmac_options)
add_test(NAME daemon.usermap COMMAND test_usermap)

add_executable(test_usermap_file test_usermap_file.c)
target_include_directories(test_usermap_file PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/daemon)
target_link_libraries(test_usermap_file PRIVATE ntfsmac_daemon ntfsmac_options)
add_test(NAME daemon.usermap_file COMMAND test_usermap_file ${CMAKE_CURRENT_BINARY_DIR}/usermap_scratch)

# The tests set the test seams of spawn_config.h, which a product build does not have
# (NTFSMAC_TEST_SEAMS=OFF, preset `product`); that build compiles the library and runs no spawn test.
if(NOT NTFSMAC_TEST_SEAMS)
  return()
endif()

# The stand-in checks the state of its own process (descriptors, signal handlers), which a
# sanitizer runtime changes, so it is built with the warning flags but without the sanitizers.
add_executable(spawn_stub spawn_stub.c)
target_compile_options(spawn_stub PRIVATE -Wall -Wextra -Werror)

add_executable(test_spawn test_spawn.c)
target_include_directories(test_spawn PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/daemon)
target_compile_definitions(test_spawn PRIVATE SPAWN_STUB_PATH="$<TARGET_FILE:spawn_stub>")
target_link_libraries(test_spawn PRIVATE ntfsmac_daemon ntfsmac_options)
add_dependencies(test_spawn spawn_stub)
add_test(NAME daemon.spawn COMMAND test_spawn)

# T-M4-06: how the spawn module applies the sandbox profiles (U-SBX-01..07, 10..12), with a shell script
# in place of sandbox-exec, and the rules the profile files must keep (U-SBX-08). What the
# profiles allow and refuse is tested in a macOS guest (tests/integration/daemon/sandbox_vm.sh).
add_executable(test_sandbox test_sandbox.c)
target_include_directories(test_sandbox PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/daemon)
target_compile_definitions(test_sandbox PRIVATE SPAWN_STUB_PATH="$<TARGET_FILE:spawn_stub>")
target_link_libraries(test_sandbox PRIVATE ntfsmac_daemon ntfsmac_options)
add_dependencies(test_sandbox spawn_stub)
add_test(NAME daemon.sandbox COMMAND test_sandbox)
add_test(NAME sandbox.profiles COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/sandbox_lint.sh
         ${PROJECT_SOURCE_DIR}/integration/sandbox)
# U-SBX-09: the lint is tested with mutated copies of the profiles.
add_test(NAME sandbox.lint_mutants COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/sandbox_lint_test.sh
         ${CMAKE_CURRENT_SOURCE_DIR}/sandbox_lint.sh ${PROJECT_SOURCE_DIR}/integration/sandbox)

# U-SBX-12: the test seams are not in spawn_config.h unless NTFSMAC_TEST_SEAMS is defined, so a build
# without it (preset `product`) cannot turn the sandbox off, point it at another tool or directory,
# or change the worker's vector. The first test must fail to compile; the second shows why.
add_test(NAME sandbox.seams_hidden
         COMMAND ${CMAKE_C_COMPILER} -fsyntax-only -I${PROJECT_SOURCE_DIR}/include
                 -I${PROJECT_SOURCE_DIR}/src/daemon ${CMAKE_CURRENT_SOURCE_DIR}/seam_leak.c)
set_tests_properties(sandbox.seams_hidden PROPERTIES WILL_FAIL TRUE)
add_test(NAME sandbox.seams_visible
         COMMAND ${CMAKE_C_COMPILER} -fsyntax-only -DNTFSMAC_TEST_SEAMS -I${PROJECT_SOURCE_DIR}/include
                 -I${PROJECT_SOURCE_DIR}/src/daemon ${CMAKE_CURRENT_SOURCE_DIR}/seam_leak.c)
