# SPDX-License-Identifier: GPL-2.0-or-later
#
# Test tools. Created by T-M2-07; T-M3-04 appends its tools below.

add_executable(treedump treedump.c)
target_link_libraries(treedump PRIVATE ntfsmac_options)

add_test(NAME tools.treedump
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_treedump.sh $<TARGET_FILE:treedump>)

# ---- T-M3-04: fixture image tools ----------------------------------------
# setvolflags, logfile_mark and txf_make edit NTFS image files through
# libntfs-3g (Environment A: the pinned build from scripts/build-ntfs3g.sh,
# found through PKG_CONFIG_PATH or CMAKE_PREFIX_PATH). Without the library, as on
# a plain host checkout, they are skipped, so the host build does not need it.
# hiber_make.sh needs no build; tests/fixtures/gen/unsafe.sh compiles the tools
# itself inside the container.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(NTFS3G QUIET IMPORTED_TARGET libntfs-3g)
endif()
if(NTFS3G_FOUND)
  foreach(tool setvolflags logfile_mark txf_make)
    add_executable(${tool} ${tool}.c)
    target_link_libraries(${tool} PRIVATE ntfsmac_options PkgConfig::NTFS3G)
  endforeach()

  # The update-sequence-array code of logfile_mark against libntfs-3g's own
  # ntfs_mst_pre_write_fixup() / ntfs_mst_post_read_fixup(); needs no image.
  add_test(NAME tools.logfile_mark_selftest COMMAND logfile_mark --selftest)
else()
  message(STATUS "libntfs-3g not found: skipping setvolflags, logfile_mark, txf_make")
endif()
