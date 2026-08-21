# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda.
# SPDX-License-Identifier: AGPL-3.0-only
#
# Run a test binary and capture its stdout/stderr to files.
#
# The test suite compares a program's output against committed golden files, so
# the output has to be redirected.  add_test() cannot redirect, and the previous
# `sh -c "prog > out 2> err"` needs a POSIX shell that Windows does not have.
# execute_process() does the same job with no shell at all.
#
# Usage from add_test():
#   ${CMAKE_COMMAND} -DIPA_TEST_CMD=<exe> [-DIPA_TEST_ARGS=<a;b;c>]
#                    -DIPA_TEST_OUT=<file> -DIPA_TEST_ERR=<file>
#                    -P .../RunAndCapture.cmake

if(NOT DEFINED IPA_TEST_CMD)
  message(FATAL_ERROR "RunAndCapture: IPA_TEST_CMD is required")
endif()

if(NOT DEFINED IPA_TEST_ARGS)
  set(IPA_TEST_ARGS "")
endif()

execute_process(
  COMMAND "${IPA_TEST_CMD}" ${IPA_TEST_ARGS}
  OUTPUT_FILE "${IPA_TEST_OUT}"
  ERROR_FILE  "${IPA_TEST_ERR}"
  RESULT_VARIABLE _rc)

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "${IPA_TEST_CMD} failed (exit ${_rc}); see ${IPA_TEST_ERR}")
endif()
