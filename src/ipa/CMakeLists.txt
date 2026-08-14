# Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

add_subdirectory(libasn)
add_subdirectory(libipa)

# libasn's own warning-suppression and SYSTEM-include settings now live in
# src/ipa/libasn/CMakeLists.txt (a hand-written, committed file), next to the
# target they configure.

find_package(OpenSSL REQUIRED)

if(IPA_TARGET_ANDROID)
  # -------------------------------------------------------------------------
  # Android (ANDROID_PORT_PLAN.md, Phase 0): one shared native core,
  # libipacore.so, later consumed by the daemon and the APK over JNI.  No
  # PC/SC (scard.c) and no command-line executable -- main.c is the Linux
  # entry point.  The eUICC backend is scard_android.c, which in Phase 0 is a
  # not-yet-implemented stub (the JNI bridge lands in Phase 1); the .so still
  # links and exposes the full core + net + crypto, with no eUICC yet.
  #
  # BoringSSL (OpenSSL-family) and libcurl are provided by the integrator for
  # the target ABI; point CMake at them via the NDK toolchain's
  # CMAKE_FIND_ROOT_PATH (or a superbuild) so find_package resolves them.
  # -------------------------------------------------------------------------
  find_package(CURL REQUIRED)

  # log_jni.c exposes the Phase-3 log sinks to the APK: the UI installs the
  # ring-buffer sink and polls it for the live log view.
  #
  # android_spike.c is a Phase-1 on-hardware diagnostic (the eUICC 61xx-chaining
  # spike, driven from the APK); it links into the same .so but is not part of
  # the production flow.  See src/ipa/android_spike.c and android/.
  add_library(libipacore SHARED http.c scard_android.c scard_jni.c log_jni.c android_spike.c)
  set_property(TARGET libipacore PROPERTY C_STANDARD 99)
  target_compile_options(libipacore PRIVATE -Wall)
  target_include_directories(libipacore PUBLIC ${CMAKE_SOURCE_DIR}/include)
  # Emit the file as libipacore.so (not liblibipacore.so): the target is named
  # "libipacore" for readability, but Android's System.loadLibrary("ipacore")
  # expects exactly lib<name>.so, so strip the doubled prefix via OUTPUT_NAME.
  set_target_properties(libipacore PROPERTIES OUTPUT_NAME ipacore)

  # Whole-archive libipa so the entire public ipad.h API (and everything it
  # reaches) survives into the shared object even though no source in this
  # target references it yet.  libasn is pulled transitively via libipa but is
  # listed for clarity; http.c / scard_android.c compiled directly above
  # provide the ipa_http_* / ipa_scard_* symbols the core links against.
  target_link_libraries(libipacore PRIVATE
    -Wl,--whole-archive libipa -Wl,--no-whole-archive
    libasn
    OpenSSL::SSL OpenSSL::Crypto
    CURL::libcurl)
else()
  add_library(http STATIC http.c)
  add_library(scard STATIC scard.c)

  target_include_directories(http PUBLIC ${CMAKE_SOURCE_DIR}/include)
  target_include_directories(scard PUBLIC ${CMAKE_SOURCE_DIR}/include)

  target_link_libraries(http PUBLIC OpenSSL::SSL OpenSSL::Crypto)

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
endif()
