# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda.
# SPDX-License-Identifier: AGPL-3.0-only
#
# Compiler-flag abstraction.
#
# The build was written against GCC/Clang and hard-codes their spellings
# (-Wall, -w, -g, -m32, -fsanitize=address).  MSVC understands none of them and
# treats an unknown /-less argument as a source file name, so they cannot just
# be passed through.  This module resolves each one into a variable that the
# CMakeLists files use in place of the literal flag; on GCC/Clang every
# variable resolves to exactly what was hard-coded before, so nothing changes
# for the existing platforms.
#
# Provides:
#   IPA_C_WARN        enable the usual warnings          (-Wall      / /W3)
#   IPA_C_NOWARN      silence a target completely        (-w         / /w)
#   IPA_C_STANDARD    C standard to request per target   (99         / 11)
#
# and applies project-wide debug-info, sanitizer and MSVC-hygiene settings.

if(MSVC)
  set(IPA_C_WARN   /W3)
  set(IPA_C_NOWARN /w)

  # MSVC has no C99 mode: /std:c99 does not exist, and without /std:c11 cl.exe
  # falls back to a C89-era dialect that rejects `inline` (utils.h) and the
  # designated array initialisers in libipa/log.c.  C11 is the lowest setting
  # that compiles the tree.  Requires VS 2019 16.8 or newer.
  set(IPA_C_STANDARD 11)

  # Only some targets set C_STANDARD explicitly; the rest (libasn, libipa,
  # http, scard) would otherwise inherit cl.exe's default dialect and fail on
  # the same constructs.  Set the floor globally -- but only here, so the
  # GCC/Clang builds keep compiling exactly the flags they did before.
  set(CMAKE_C_STANDARD 11)
  set(CMAKE_C_STANDARD_REQUIRED ON)

  # The sources are UTF-8 without a BOM: 44 of them carry a section sign, an
  # arrow or an em dash in spec-reference comments.  Left to itself cl.exe
  # decodes them in the machine's ANSI code page, which warns (C4819) and can
  # swallow the newline ending a comment.  /utf-8 is shorthand for
  # /source-charset:utf-8 /execution-charset:utf-8.
  add_compile_options(/utf-8)

  # fopen, snprintf, getenv and friends are "deprecated" by the MS CRT in
  # favour of its _s variants.  The code is portable C; silence C4996 rather
  # than fork every call site.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)

  # Keep <windows.h> from defining min/max as macros; several dependencies
  # (and asn1c's asn_system.h) pull it in transitively.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  set(IPA_C_WARN   -Wall)
  set(IPA_C_NOWARN -w)
  set(IPA_C_STANDARD 99)
endif()

# --- Debug info ------------------------------------------------------------
# src/CMakeLists.txt used to add -g unconditionally, independent of build type.
# Preserve that intent on both toolchains.
if(MSVC)
  add_compile_options(/Zi)
  add_link_options(/DEBUG)
else()
  add_compile_options(-g)
  add_link_options(-g)
endif()

# --- Address sanitizer -----------------------------------------------------
function(ipa_enable_sanitizer)
  if(MSVC)
    # MSVC has had ASan since VS 2019 16.9.  It is incompatible with
    # incremental linking and with the /RTC runtime checks CMake puts in Debug
    # builds, so clear those rather than let the link fail confusingly.
    add_compile_options(/fsanitize=address)
    string(REGEX REPLACE "/RTC[1csu]*" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}" PARENT_SCOPE)
    add_link_options(/INCREMENTAL:NO)
  else()
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
  endif()
endfunction()

# --- 32 bit builds ---------------------------------------------------------
# -m32 is a GCC/Clang concept.  With MSVC the target architecture belongs to
# the generator (cmake -A Win32), so M32 cannot be honoured after the fact.
function(ipa_apply_m32 target)
  if(NOT M32)
    return()
  endif()
  if(MSVC)
    message(FATAL_ERROR
      "M32=ON is not supported with MSVC.  Select the 32 bit target at "
      "configure time instead: cmake -A Win32 ...")
  endif()
  set_property(TARGET ${target} APPEND_STRING PROPERTY COMPILE_FLAGS " -m32")
  set_property(TARGET ${target} APPEND_STRING PROPERTY LINK_FLAGS " -m32")
endfunction()
