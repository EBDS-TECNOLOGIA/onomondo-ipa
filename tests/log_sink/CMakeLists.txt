# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

add_executable(log_sink_test log_sink_test.c)
set_property(TARGET log_sink_test PROPERTY C_STANDARD 99)
target_compile_options(log_sink_test PRIVATE -Wall)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/include)

target_link_libraries(log_sink_test libipa)
if (M32)
  set_target_properties(log_sink_test PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()

add_test(NAME log_sink_test
    COMMAND sh -c "$<TARGET_FILE:log_sink_test>")
