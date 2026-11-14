# SPDX-License-Identifier: GPL-2.0-or-later
#
# Worker option builder and argv_check(). The static library is named
# ntfsmac_worker_options because the target ntfsmac_options is the compile
# flags interface of cmake/NtfsmacFlags.cmake.

add_library(ntfsmac_worker_options STATIC
  options.c
  argv_check.c)
target_include_directories(ntfsmac_worker_options PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ntfsmac_worker_options PUBLIC ntfsmac_policy)
target_link_libraries(ntfsmac_worker_options PRIVATE ntfsmac_options)
