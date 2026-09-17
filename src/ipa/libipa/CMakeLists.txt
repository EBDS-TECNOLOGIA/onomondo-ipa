# Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only

add_library(libipa STATIC
  activation_code.c
  bpp_segments.c
  cert.c
  config_json.c
  device_info.c
  esipa_json.c
  es10a_get_euicc_cfg_addr.c
  es10b_add_init_eim.c
  es10b_auth_serv.c
  es10b_cancel_session.c
  es10b_euicc_mem_rst.c
  es10b_get_certs.c
  es10b_get_eim_cfg_data.c
  es10b_get_euicc_chlg.c
  es10b_get_euicc_info.c
  es10b_get_rat.c
  es10b_load_bnd_prfle_pkg.c
  es10b_load_euicc_pkg.c
  es10b_prep_dwnld.c
  es10b_prfle_rollback.c
  es10b_retr_notif_from_lst.c
  es10b_rm_notif_from_lst.c
  es10c_delete_prfle.c
  es10c_disable_prfle.c
  es10c_enable_prfle.c
  es10b_immediate_enable.c
  es10b_execute_fallback.c
  ipae_activation.c
  es10b_cfg_immediate_enable.c
  es10b_return_from_fallback.c
  es10b_enable_emergency_profile.c
  es10b_disable_emergency_profile.c
  es10b_get_connectivity_params.c
  es10b_set_default_dp_addr.c
  es10c_get_eid.c
  es10c_get_prfle_info.c
  es10x.c
  esipa_auth_clnt.c
  esipa.c
  esipa_cancel_session.c
  esipa_get_bnd_prfle_pkg.c
  esipa_get_eim_pkg.c
  esipa_handle_notif.c
  esipa_init_auth.c
  esipa_prvde_eim_pkg_rslt.c
  euicc.c
  fileio.c
  ipad.c
  log.c
  log_file_sink.c
  log_ring_sink.c
  ppr.c
  proc_cmn_cancel_sess.c
  proc_cmn_mtl_auth.c
  proc_indirect_prfle_dwnld.c
  proc_eim_pkg_retr.c
  proc_euicc_data_req.c
  proc_euicc_pkg_dwnld_exec.c
  proc_notif_delivery.c
  proc_prfle_dwnld.c
  proc_prfle_inst.c
  run.c
  utils.c
)

target_include_directories(libipa PUBLIC ${CMAKE_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(libipa PUBLIC libasn)

# The log sinks use a pthread mutex: the IPAd's poll loop logs from its own
# thread while the APK's UI drains the ring buffer from another.  On Android
# and on modern glibc pthreads live in libc, so this usually resolves to
# nothing at all.
find_package(Threads REQUIRED)
target_link_libraries(libipa PUBLIC Threads::Threads)

# syslog sink (onomondo/ipa/log_syslog.h), for daemons on POSIX hosts.
if(UNIX)
  target_sources(libipa PRIVATE log_syslog_sink.c)
endif()
target_compile_options(libipa PRIVATE -Wall)

# The IoT eUICC emulation (see the top-level CMakeLists.txt).  PUBLIC because the sample application, the tests and
# any other consumer of libipa have to agree with the library about whether the emulation exists -- ipa_config keeps
# the runtime flag either way, so the difference is not visible in the struct layout.
if(IOT_EUICC_EMULATION)
  target_compile_definitions(libipa PUBLIC IPA_HAVE_IOT_EUICC_EMULATION=1)
endif()

# The two ESipa wire bindings (see the top-level CMakeLists.txt for the options).  PUBLIC, because the sample
# application, the tests and any other consumer have to agree with the library about which bindings exist.
if(ESIPA_BINDING_ASN1)
  target_compile_definitions(libipa PUBLIC IPA_HAVE_ESIPA_ASN1=1)
endif()

# jansson serves two consumers: the ESipa JSON binding and the JSON configuration file read by ipa_run() (see
# onomondo/ipa/config_json.h).  It is probed once for both.  The configuration file is optional -- without jansson
# ipa_config_json_load() fails loudly -- so only the binding turns a missing jansson into a configure error.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(JANSSON QUIET jansson)
endif()
# Fallback probe: pkg-config is not installed everywhere (and never under the
# NDK toolchain), so look for the header and the library directly before
# concluding that jansson is missing.  Under a cross toolchain these searches
# stay inside the sysroot (CMAKE_FIND_ROOT_PATH_MODE_INCLUDE/LIBRARY = ONLY),
# so they cannot pick up the host copy -- for Android the integrator supplies
# jansson for the target ABI the same way as BoringSSL and libcurl.
if(NOT JANSSON_FOUND)
  find_path(JANSSON_INCLUDE_DIR NAMES jansson.h)
  find_library(JANSSON_LIBRARY NAMES jansson)
  if(JANSSON_INCLUDE_DIR AND JANSSON_LIBRARY)
    set(JANSSON_FOUND TRUE)
    set(JANSSON_INCLUDE_DIRS ${JANSSON_INCLUDE_DIR})
    set(JANSSON_LIBRARIES ${JANSSON_LIBRARY})
  endif()
endif()
if(JANSSON_FOUND)
  message(STATUS "jansson found; JSON configuration file supported")
  target_compile_definitions(libipa PRIVATE IPA_HAVE_JANSSON=1)
  target_include_directories(libipa PRIVATE ${JANSSON_INCLUDE_DIRS})
  target_link_libraries(libipa PUBLIC ${JANSSON_LIBRARIES})
  target_link_directories(libipa PUBLIC ${JANSSON_LIBRARY_DIRS})
else()
  message(STATUS "jansson not found; JSON configuration file NOT supported")
endif()

# The JSON binding needs jansson.  ESIPA_BINDING_JSON is ON by default, and when it is on the dependency is a hard
# requirement: silently degrading to an ASN.1-only library because the build host happened not to have
# libjansson-dev is how two machines end up producing two different libipa from the same source.
if(ESIPA_BINDING_JSON)
  if(NOT JANSSON_FOUND)
    message(FATAL_ERROR
      "ESipa binding JSON (section 6.4) is enabled but jansson was not found. "
      "Install it (e.g. 'apt-get install libjansson-dev'), or build without the JSON binding "
      "by passing -DESIPA_BINDING_JSON=OFF.")
  endif()
  message(STATUS "ESipa binding JSON (section 6.4): built (jansson ${JANSSON_VERSION})")
  target_compile_definitions(libipa PUBLIC IPA_HAVE_ESIPA_JSON=1)
else()
  message(STATUS "ESipa binding JSON (section 6.4): NOT built")
endif()
if (M32)
  set_target_properties(libipa PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()
