# Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

add_subdirectory(libasn)
add_subdirectory(libipa)

# libasn's own warning-suppression and SYSTEM-include settings now live in
# src/ipa/libasn/CMakeLists.txt (a hand-written, committed file), next to the
# target they configure.

# The TLS library behind the eUICC trust anchor handling in http.c (see http_tls.h). It has to be the one libcurl
# is built against: OpenSSL on most distributions, Mbed TLS on OpenWrt.
set(IPA_HTTP_TLS "openssl" CACHE STRING "TLS library libcurl is built against: openssl or mbedtls")
set_property(CACHE IPA_HTTP_TLS PROPERTY STRINGS openssl mbedtls)
if(IPA_HTTP_TLS STREQUAL "openssl")
  find_package(OpenSSL REQUIRED)
  set(IPA_HTTP_TLS_SRC http_tls_openssl.c)
  set(IPA_HTTP_TLS_LIBS OpenSSL::SSL OpenSSL::Crypto)
elseif(IPA_HTTP_TLS STREQUAL "mbedtls")
  find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h)
  find_library(MBEDTLS_LIBRARY mbedtls)
  find_library(MBEDX509_LIBRARY mbedx509)
  find_library(MBEDCRYPTO_LIBRARY mbedcrypto)
  if(NOT MBEDTLS_INCLUDE_DIR OR NOT MBEDTLS_LIBRARY OR NOT MBEDX509_LIBRARY OR NOT MBEDCRYPTO_LIBRARY)
    message(FATAL_ERROR "IPA_HTTP_TLS=mbedtls, but the Mbed TLS headers or libraries were not found")
  endif()
  set(IPA_HTTP_TLS_SRC http_tls_mbedtls.c)
  set(IPA_HTTP_TLS_LIBS ${MBEDTLS_LIBRARY} ${MBEDX509_LIBRARY} ${MBEDCRYPTO_LIBRARY})
else()
  message(FATAL_ERROR "IPA_HTTP_TLS must be openssl or mbedtls, not '${IPA_HTTP_TLS}'")
endif()
message(STATUS "HTTP client TLS library: ${IPA_HTTP_TLS}")

add_library(http STATIC http.c ${IPA_HTTP_TLS_SRC})
add_library(scard STATIC scard.c)

target_include_directories(http PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(scard PUBLIC ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(http PUBLIC ${IPA_HTTP_TLS_LIBS})
if(IPA_HTTP_TLS STREQUAL "mbedtls")
  target_include_directories(http PRIVATE ${MBEDTLS_INCLUDE_DIR})
endif()

if(APPLE)
  find_path(PCSC_INCLUDE_DIR NAMES winscard.h PATHS /opt/homebrew/opt/pcsc-lite/include/PCSC)
  include_directories(include ${PCSC_INCLUDE_DIR})
  find_library(PCSC_LIBRARY PCSC)
else()
  include_directories(include /usr/include/PCSC)
  set(PCSC_LIBRARY pcsclite)
endif()

add_executable(ipa main.c)
set_property(TARGET ipa PROPERTY C_STANDARD 99)
target_compile_options(ipa PRIVATE -Wall)

target_include_directories(ipa PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ipa libipa http scard ${PCSC_LIBRARY} curl)

if (M32)
  set_target_properties(http PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
  set_target_properties(scard PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
  set_target_properties(ipa PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()

# Linux daemon and cross-build fixes, kept in their own file (see there).
include(${CMAKE_CURRENT_SOURCE_DIR}/linux_daemon.cmake)
