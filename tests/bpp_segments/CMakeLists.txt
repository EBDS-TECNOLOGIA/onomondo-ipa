# Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

add_executable(bpp_segments_test bpp_segments_test.c)
set_property(TARGET bpp_segments_test PROPERTY C_STANDARD ${IPA_C_STANDARD})
target_compile_options(bpp_segments_test PRIVATE ${IPA_C_WARN})

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/include)

target_link_libraries(bpp_segments_test libipa)
ipa_apply_m32(bpp_segments_test)

# CMake 3.14+ can compare ignoring line-ending differences, which is what makes
# the LF golden files usable against a CRLF run on Windows.
set(_cmp_eol "")
if(NOT CMAKE_VERSION VERSION_LESS 3.14)
  set(_cmp_eol --ignore-eol)
endif()

add_test(NAME bpp_segments_test
    COMMAND ${CMAKE_COMMAND}
    -DIPA_TEST_CMD=$<TARGET_FILE:bpp_segments_test>
    -DIPA_TEST_ARGS=${CMAKE_CURRENT_SOURCE_DIR}/bpp.ber
    -DIPA_TEST_OUT=${CMAKE_CURRENT_BINARY_DIR}/bpp_segments_test.out
    -DIPA_TEST_ERR=${CMAKE_CURRENT_BINARY_DIR}/bpp_segments_test.err
    -P ${PROJECT_SOURCE_DIR}/cmake/RunAndCapture.cmake)

add_test(NAME bpp_segments_compare_stdout
    COMMAND ${CMAKE_COMMAND} -E compare_files ${_cmp_eol}
    ${CMAKE_CURRENT_BINARY_DIR}/bpp_segments_test.out
    ${CMAKE_CURRENT_SOURCE_DIR}/bpp_segments_test.ok)

add_test(NAME bpp_segments_compare_stderr
    COMMAND ${CMAKE_COMMAND} -E compare_files ${_cmp_eol}
    ${CMAKE_CURRENT_BINARY_DIR}/bpp_segments_test.err
    ${CMAKE_CURRENT_SOURCE_DIR}/bpp_segments_test.err)

# The comparisons read files the run above produces, so make the ordering
# explicit rather than relying on ctest's declaration order (which does not
# hold under `ctest -j`).
set_tests_properties(
    bpp_segments_compare_stdout
    bpp_segments_compare_stderr
    PROPERTIES DEPENDS bpp_segments_test)
