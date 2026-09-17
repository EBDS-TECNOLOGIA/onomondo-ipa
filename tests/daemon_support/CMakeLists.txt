# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

# Support code of the Linux daemon (OPENWRT_PORT_ANALYSIS.md, section 7): the syslog sink, the atomic nvstate
# save and its write-on-change rule, and the ipad binary itself.

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/include)

add_executable(log_syslog_test log_syslog_test.c)
set_property(TARGET log_syslog_test PROPERTY C_STANDARD 99)
target_compile_options(log_syslog_test PRIVATE -Wall)
target_link_libraries(log_syslog_test libipa)
add_test(NAME log_syslog_test COMMAND log_syslog_test)

add_executable(config_ext_test config_ext_test.c)
set_property(TARGET config_ext_test PROPERTY C_STANDARD 99)
target_compile_options(config_ext_test PRIVATE -Wall)
target_link_libraries(config_ext_test libipa)
add_test(NAME config_ext_test COMMAND config_ext_test)

add_executable(nvstate_save_test nvstate_save_test.c)
set_property(TARGET nvstate_save_test PROPERTY C_STANDARD 99)
target_compile_options(nvstate_save_test PRIVATE -Wall)
target_link_libraries(nvstate_save_test libipa)
add_test(NAME nvstate_save_test COMMAND nvstate_save_test)

if(TARGET ipad)
  add_test(NAME ipad_daemon_test
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/ipad_daemon_test.sh $<TARGET_FILE:ipad>)
endif()

if (M32)
  set_target_properties(log_syslog_test config_ext_test nvstate_save_test PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()
