# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only
#
# Additions for the OpenWrt port (OPENWRT_PORT_ANALYSIS.md), included at the end of CMakeLists.txt. They are kept
# in this file, rather than edited into CMakeLists.txt, so that file merges with the other ports with a
# one-line difference.

# Nothing to add where the Linux CLI and its PC/SC backend are not built: macOS, and the Android port, whose
# CMakeLists.txt this line may one day be merged into.
if(APPLE OR NOT TARGET scard OR NOT TARGET ipa)
  return()
endif()

# PC/SC headers from pkg-config. Under a cross toolchain (the OpenWrt SDK) pkg-config answers from the target
# sysroot, whereas CMakeLists.txt adds the build host's /usr/include/PCSC. Putting the pkg-config directories
# first makes the target's winscard.h win; the host path stays behind them, where it only matters for a native
# build without pkg-config.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PCSCLITE QUIET libpcsclite)
endif()
if(PCSCLITE_FOUND)
  target_include_directories(scard BEFORE PRIVATE ${PCSCLITE_INCLUDE_DIRS})
endif()

# The eUICC transport is chosen at run time (onomondo/ipa/scard_transport.h): PC/SC as before, or a modem's
# AT+CSIM. scard_dispatch.c provides the ipa_scard_* functions the core calls and forwards them; scard.c keeps
# its own source unchanged and is compiled with its names mapped to ipa_scard_pcsc_*, so that file still merges
# with the Android and Windows ports.
target_sources(scard PRIVATE scard_dispatch.c scard_at.c)
# The rename is per source file: scard_dispatch.c and scard_at.c must see the real names.
set_source_files_properties(scard.c PROPERTIES COMPILE_DEFINITIONS
  "ipa_scard_init=ipa_scard_pcsc_init;ipa_scard_reset=ipa_scard_pcsc_reset;ipa_scard_atr=ipa_scard_pcsc_atr;ipa_scard_transceive=ipa_scard_pcsc_transceive;ipa_scard_free=ipa_scard_pcsc_free")
# Front ends can select a transport, and know they can; the tests check whether it was built.
target_compile_definitions(ipa PRIVATE IPA_HAVE_TRANSPORT_URI=1)
set(IPA_HAVE_TRANSPORT_URI ON CACHE INTERNAL "the eUICC transport dispatcher is built")
set(IPA_PCSC_LIBRARY ${PCSC_LIBRARY} CACHE INTERNAL "PC/SC library the scard target needs")

# ipad: the daemon (ipad_linux.c), for OpenWrt and other supervised Linux hosts. Same backends as the CLI.
add_executable(ipad ipad_linux.c)
set_property(TARGET ipad PROPERTY C_STANDARD 99)
target_compile_options(ipad PRIVATE -Wall)
target_compile_definitions(ipad PRIVATE IPAD_VERSION="${PROJECT_VERSION}")
target_include_directories(ipad PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ipad libipa http scard ${PCSC_LIBRARY} curl)
if(PCSCLITE_FOUND)
  target_link_directories(ipa PRIVATE ${PCSCLITE_LIBRARY_DIRS})
  target_link_directories(ipad PRIVATE ${PCSCLITE_LIBRARY_DIRS})
endif()
if(M32)
  set_target_properties(ipad PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()

install(TARGETS ipa RUNTIME DESTINATION bin)
install(TARGETS ipad RUNTIME DESTINATION sbin)
