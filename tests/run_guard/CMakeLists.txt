# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

add_executable(run_guard_test run_guard_test.c)
set_property(TARGET run_guard_test PROPERTY C_STANDARD 99)
target_compile_options(run_guard_test PRIVATE -Wall)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/include)

# libipa only -- the scard/http backends are stubbed inside the test.
target_link_libraries(run_guard_test libipa)
if (M32)
  set_target_properties(run_guard_test PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()

add_test(NAME run_guard_test
    COMMAND sh -c "$<TARGET_FILE:run_guard_test>")
