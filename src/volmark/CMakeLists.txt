# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-12: ntfsmac-volmark, the on-disk dirty marker (docs/ARCHITECTURE.md section 3.6).
#
# The pure part (command line and the step-3 flag decision, args.c) always builds and is unit
# tested on every host. The tool itself needs libntfs-3g (the static library of
# scripts/build-ntfs3g.sh, found through PKG_CONFIG_PATH); without it, as on a plain host
# checkout, it is skipped. Environment A builds it (tests/unit/volmark/run.sh), once against the
# release library and once against the --enable-faultio library (patch 0004) for the cut tests.
# T-M4-16: syncmirror.c, the `sync-mirror` mode (docs/ARCHITECTURE.md section 3.7).

add_library(ntfsmac_volmark_pure STATIC args.c)
target_include_directories(ntfsmac_volmark_pure PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ntfsmac_volmark_pure PRIVATE ntfsmac_options)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(NTFS3G QUIET IMPORTED_TARGET libntfs-3g)
endif()

if(NTFS3G_FOUND)
  add_library(ntfsmac_volmark_core STATIC volmark.c syncmirror.c vdev.c cli.c)
  target_include_directories(ntfsmac_volmark_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ntfsmac_volmark_core
    PRIVATE ntfsmac_options
    PUBLIC ntfsmac_volmark_pure ntfsmac_mftmirr PkgConfig::NTFS3G)
  if(APPLE)
    # As the probe: libntfs-3g built with nfconv references CoreFoundation.
    target_link_libraries(ntfsmac_volmark_core PUBLIC "-framework CoreFoundation")
  endif()

  add_executable(ntfsmac-volmark main.c)
  target_link_libraries(ntfsmac-volmark PRIVATE ntfsmac_volmark_core ntfsmac_options)

  # Test build: the same code plus --stop-before-write and --no-alarm. Not installed.
  add_executable(volmark_test main_test.c)
  target_link_libraries(volmark_test PRIVATE ntfsmac_volmark_core ntfsmac_options)
else()
  message(STATUS "libntfs-3g not found: skipping ntfsmac-volmark")
endif()
