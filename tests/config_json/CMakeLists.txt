# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

add_executable(config_json_test config_json_test.c)
set_property(TARGET config_json_test PROPERTY C_STANDARD 99)
target_compile_options(config_json_test PRIVATE -Wall)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/include)

target_compile_definitions(config_json_test PRIVATE
  IPA_EXAMPLE_CONFIG="${PROJECT_SOURCE_DIR}/contrib/ipa-config.example.json")

target_link_libraries(config_json_test libipa)
if (M32)
  set_target_properties(config_json_test PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()

add_test(NAME config_json_test
    COMMAND sh -c "$<TARGET_FILE:config_json_test>")
