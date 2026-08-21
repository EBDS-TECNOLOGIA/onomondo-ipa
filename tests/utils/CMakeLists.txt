# Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

add_executable(utils_test utils_test.c)
set_property(TARGET utils_test PROPERTY C_STANDARD ${IPA_C_STANDARD})
target_compile_options(utils_test PRIVATE ${IPA_C_WARN})

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/include)

target_link_libraries(utils_test libipa)
ipa_apply_m32(utils_test)

add_test(NAME utils_test COMMAND utils_test)
