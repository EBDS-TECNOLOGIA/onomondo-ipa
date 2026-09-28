# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

add_executable(es10x_chaining_test es10x_chaining_test.c)
set_property(TARGET es10x_chaining_test PROPERTY C_STANDARD 99)
target_compile_options(es10x_chaining_test PRIVATE -Wall)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/include)

target_link_libraries(es10x_chaining_test libipa)
if (M32)
  set_target_properties(es10x_chaining_test PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()

add_test(NAME es10x_chaining_test
    COMMAND sh -c "$<TARGET_FILE:es10x_chaining_test> > es10x_chaining_test.out 2> es10x_chaining_test.err")

add_test(NAME es10x_chaining_compare_stdout
    COMMAND ${CMAKE_COMMAND} -E compare_files
    ${PROJECT_SOURCE_DIR}/build/tests/es10x_chaining/es10x_chaining_test.out
    ${CMAKE_CURRENT_SOURCE_DIR}/es10x_chaining_test.ok)
