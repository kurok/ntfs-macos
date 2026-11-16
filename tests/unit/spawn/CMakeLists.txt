# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-01: tests of the spawn module (U-SPN-*). spawn_stub is a stand-in for the
# probe, volmark and the worker; its behaviour is chosen by the base name of
# the path it is started under (the test makes one symlink per behaviour).

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
