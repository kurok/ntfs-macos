# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test tools. Created by T-M2-07; T-M3-04 appends its tools below.

add_executable(treedump treedump.c)
target_link_libraries(treedump PRIVATE ntfsmac_options)

add_test(NAME tools.treedump
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_treedump.sh $<TARGET_FILE:treedump>)
