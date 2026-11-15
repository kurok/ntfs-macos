# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-02: ntfsmac-probe (docs/ARCHITECTURE.md section 3.1).
#
# The pure parts (NFC, state derivation, UTF-8 text, JSON writer) always build and are
# unit tested on every host. The probe itself needs libntfs-3g
# (the static library of scripts/build-ntfs3g.sh, found through PKG_CONFIG_PATH);
# without it, as on a plain host checkout, it is skipped and the host build stays
# independent of the dependency. Environment A builds it (tests/unit/probe/run.sh).

add_library(ntfsmac_probe_pure STATIC nfc.c state.c text.c json.c)
target_include_directories(ntfsmac_probe_pure PUBLIC ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ntfsmac_probe_pure PUBLIC ntfsmac_state_rank PRIVATE ntfsmac_options)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(NTFS3G QUIET IMPORTED_TARGET libntfs-3g)
endif()

if(NTFS3G_FOUND)
  add_library(ntfsmac_probe_core STATIC
    probe.c devio.c names.c cli.c)
  target_include_directories(ntfsmac_probe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ntfsmac_probe_core
    PRIVATE ntfsmac_options
    PUBLIC ntfsmac_probe_pure ntfsmac_mftmirr PkgConfig::NTFS3G)
  if(APPLE)
    # libntfs-3g built with nfconv references CoreFoundation (unistr.c); the probe
    # never calls that code, but the static library still needs the framework to link.
    target_link_libraries(ntfsmac_probe_core PUBLIC "-framework CoreFoundation")
  endif()

  add_executable(ntfsmac-probe main.c)
  target_link_libraries(ntfsmac-probe PRIVATE ntfsmac_probe_core ntfsmac_options)

  # Test build: the same code plus --scan-budget-ms and --no-alarm. Not installed.
  add_executable(probe_test main_test.c)
  target_link_libraries(probe_test PRIVATE ntfsmac_probe_core ntfsmac_options)
else()
  message(STATUS "libntfs-3g not found: skipping ntfsmac-probe")
endif()
