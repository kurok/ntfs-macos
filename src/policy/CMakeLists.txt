# SPDX-License-Identifier: GPL-2.0-or-later

add_library(ntfsmac_policy STATIC policy.c)
target_include_directories(ntfsmac_policy PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ntfsmac_policy PRIVATE ntfsmac_options)
