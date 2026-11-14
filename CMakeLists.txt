# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.28)

project(ntfs-macos VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Sanitizers: "", "address", "undefined" or "address,undefined".
set(NTFSMAC_SANITIZE "" CACHE STRING "Sanitizers to enable (address, undefined)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Platform: macOS arm64 is the product target; Linux builds the portable
# parts only (see docs/ARCHITECTURE.md section 10).
if(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    message(STATUS "Platform: macOS arm64 (product target)")
  else()
    message(WARNING "macOS ${CMAKE_SYSTEM_PROCESSOR}: only arm64 is a supported target")
  endif()
  set(NTFSMAC_PLATFORM_MACOS TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "Platform: Linux (portable parts only)")
  set(NTFSMAC_PLATFORM_LINUX TRUE)
else()
  message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

include(cmake/NtfsmacFlags.cmake)

enable_testing()

add_subdirectory(src/common)
add_subdirectory(tests/tools)
