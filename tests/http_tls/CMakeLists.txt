# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

include_directories(${PROJECT_SOURCE_DIR}/include)

add_executable(http_tls_test http_tls_test.c)
set_property(TARGET http_tls_test PROPERTY C_STANDARD 99)
target_compile_options(http_tls_test PRIVATE -Wall)
target_link_libraries(http_tls_test http libipa curl)

find_program(OPENSSL_PROGRAM openssl)
find_program(PYTHON3_PROGRAM python3)
if(OPENSSL_PROGRAM AND PYTHON3_PROGRAM)
  add_test(NAME http_tls_test
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/http_tls_test.sh $<TARGET_FILE:http_tls_test>)
else()
  message(STATUS "http_tls_test: openssl or python3 not found, test not registered")
endif()
