# Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

add_subdirectory(libasn)
add_subdirectory(libipa)

# libasn's own warning-suppression and SYSTEM-include settings now live in
# src/ipa/libasn/CMakeLists.txt (a hand-written, committed file), next to the
# target they configure.

find_package(OpenSSL REQUIRED)

add_library(http STATIC http.c)
add_library(scard STATIC scard.c)

target_include_directories(http PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(scard PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(http PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# --- PC/SC ----------------------------------------------------------------
# scard.c is written against the WinSCard API.  Windows implements it natively
# in winscard.dll (the API pcsc-lite clones), so there is nothing to find: link
# the import library and use the Windows SDK headers.  Elsewhere it comes from
# pcsc-lite, or from the PCSC framework on macOS.
if(WIN32)
  set(PCSC_LIBRARY winscard)
elseif(APPLE)
  find_path(PCSC_INCLUDE_DIR NAMES winscard.h PATHS /opt/homebrew/opt/pcsc-lite/include/PCSC)
  include_directories(include ${PCSC_INCLUDE_DIR})
  find_library(PCSC_LIBRARY PCSC)
else()
  include_directories(include /usr/include/PCSC)
  set(PCSC_LIBRARY pcsclite)
endif()

# --- HTTP -----------------------------------------------------------------
# libcurl is kept on Windows rather than swapped for WinHTTP: http.c installs
# the eUICC-provisioned TLS credentials (trustedCertificateTls, trustedEimPkTls
# and the eUICC-backed client key) by reaching into the OpenSSL SSL_CTX through
# CURLOPT_SSL_CTX_FUNCTION.  Schannel exposes no equivalent hook, so a WinHTTP
# backend would mean reimplementing SGP.32 chain validation against CryptoAPI.
# Requiring a libcurl built against OpenSSL keeps http.c byte-identical across
# platforms.  See WINDOWS_BUILD.md.
find_package(CURL REQUIRED)

# --- the CLI --------------------------------------------------------------
set(IPA_SOURCES main.c)
if(WIN32)
  # POSIX interfaces the MS CRT does not provide; see include/onomondo/ipa/compat.h.
  list(APPEND IPA_SOURCES compat/compat_win32.c compat/getopt_win32.c)
endif()

add_executable(ipa ${IPA_SOURCES})
set_property(TARGET ipa PROPERTY C_STANDARD ${IPA_C_STANDARD})
target_compile_options(ipa PRIVATE ${IPA_C_WARN})

target_include_directories(ipa PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ipa libipa http scard ${PCSC_LIBRARY} CURL::libcurl)

ipa_apply_m32(http)
ipa_apply_m32(scard)
ipa_apply_m32(ipa)
