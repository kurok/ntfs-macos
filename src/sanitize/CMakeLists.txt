# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-19: disk-derived strings (ADR-016, ADR-023): strict probe-verdict parser,
# label sanitiser, path display sanitiser, mount-point leaf creation. Pure C
# (leaf.c calls mkdirat). The label allow-list is the shared unit ntfsmac_label_cp
# (src/common), which the final argv validator uses as well. The state severity table
# (ntfsmac_state_rank) is not linked here: the parser does not use it.

add_library(ntfsmac_sanitize STATIC
  label.c
  display.c
  leaf.c
  verdict.c)
target_include_directories(ntfsmac_sanitize PUBLIC ${PROJECT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ntfsmac_sanitize PUBLIC ntfsmac_label_cp PRIVATE ntfsmac_options)
