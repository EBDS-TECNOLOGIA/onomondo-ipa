# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

# The AT+CSIM transport needs the dispatcher, which is only built on the hosts that have it.
if(NOT TARGET scard OR NOT IPA_HAVE_TRANSPORT_URI)
  return()
endif()

include_directories(${PROJECT_SOURCE_DIR}/include)

add_executable(scard_at_test scard_at_test.c)
set_property(TARGET scard_at_test PROPERTY C_STANDARD 99)
target_compile_options(scard_at_test PRIVATE -Wall)
# libipa for the ES10x layer, scard for the transport; the eIM side is stubbed inside the test.
target_link_libraries(scard_at_test libipa scard ${IPA_PCSC_LIBRARY})
add_test(NAME scard_at_test COMMAND scard_at_test)

if (M32)
  set_target_properties(scard_at_test PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()
