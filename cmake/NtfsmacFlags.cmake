# SPDX-License-Identifier: GPL-2.0-or-later
#
# Defines the INTERFACE target ntfsmac_options. Link it PRIVATE into every
# project target; it carries warnings, hardening and sanitizer flags.
# Third-party code (third_party/) must NOT link it, so -Werror never applies
# to upstream sources.

include(CheckCCompilerFlag)

add_library(ntfsmac_options INTERFACE)

target_compile_options(ntfsmac_options INTERFACE -Wall -Wextra -Werror)

# Hardening (docs/ARCHITECTURE.md section 6).
check_c_compiler_flag(-fstack-protector-strong NTFSMAC_HAVE_STACK_PROTECTOR_STRONG)
if(NTFSMAC_HAVE_STACK_PROTECTOR_STRONG)
  target_compile_options(ntfsmac_options INTERFACE -fstack-protector-strong)
endif()

check_c_compiler_flag(-ftrivial-auto-var-init=zero NTFSMAC_HAVE_AUTO_VAR_INIT)
if(NTFSMAC_HAVE_AUTO_VAR_INIT)
  target_compile_options(ntfsmac_options INTERFACE -ftrivial-auto-var-init=zero)
endif()

# _FORTIFY_SOURCE needs optimisation and conflicts with sanitizer runtimes.
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$" AND NOT NTFSMAC_SANITIZE)
  target_compile_options(ntfsmac_options INTERFACE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2)
endif()

if(NTFSMAC_SANITIZE)
  target_compile_options(ntfsmac_options INTERFACE
    -fsanitize=${NTFSMAC_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(ntfsmac_options INTERFACE -fsanitize=${NTFSMAC_SANITIZE})
  message(STATUS "Sanitizers: ${NTFSMAC_SANITIZE}")
endif()
