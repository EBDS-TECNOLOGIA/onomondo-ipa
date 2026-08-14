/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <onomondo/ipa/ipad.h>

/* ===========================================================================
 * JSON configuration (ANDROID_PORT_PLAN.md, Phase 2)
 * ===========================================================================
 *
 * The Linux CLI (src/ipa/main.c) configures the IPAd from command-line flags.
 * The Android daemon and the APK have no command line, so they read the same
 * settings from a JSON file instead.  This module is the single place where
 * that file is turned into a struct ipa_config plus the handful of auxiliary
 * settings that main.c currently keeps as locals (nvstate path, initial-eIM
 * configuration path, memory reset, one-package-only).
 *
 * There is exactly one JSON key per CLI flag; the mapping is documented in
 * ANDROID_PORT_PLAN.md.  Defaults are the IPA_DEFAULT_* macros below, which
 * main.c also uses, so the CLI and the config file cannot drift apart.
 *
 * Keys starting with an underscore are ignored, so a configuration file can
 * carry "_comment" annotations despite JSON having no comment syntax.
 *
 * Requires jansson (already a dependency for the ESipa v1.2 JSON binding).
 * When the library was built without jansson, ipa_config_json_load() fails
 * with a clear log line instead of silently running on defaults.
 */

/*! Default card reader number (CLI: -r). */
#define IPA_DEFAULT_READER_NUMBER 0

/*! Default ISD-R logical channel number (CLI: -c).  Ignored when the
 *  transport manages the channel itself (Android telephony / OMAPI). */
#define IPA_DEFAULT_CHANNEL_NUMBER 1

/*! Default TAC, as a hex string (CLI: -t). */
#define IPA_DEFAULT_TAC "12345678"

/*! Default path of the non-volatile state file (CLI: -n). */
#define IPA_DEFAULT_NVSTATE_PATH "./nvstate.bin"

/*! Default number of retries for ESipa requests (CLI: -y). */
#define IPA_DEFAULT_ESIPA_REQ_RETRIES 3

/*! Highest logical channel number an ISO 7816-4 CLA byte can encode. */
#define IPA_MAX_CHANNEL_NUMBER 19

/*! Everything a front-end needs to run the IPAd, as parsed from the JSON
 *  configuration file.
 *
 *  Ownership: every char * member is owned by this struct and released by
 *  ipa_run_config_free().  That includes cfg.preferred_eim_id and
 *  cfg.eim_cabundle, which point into storage owned here (struct ipa_config
 *  itself does not own its strings -- with the CLI they point at argv). */
struct ipa_run_config {
	/*! Configuration handed to ipa_new_ctx(). */
	struct ipa_config cfg;

	/*! Path of the non-volatile state file (JSON: nvstate_path). */
	char *nvstate_path;

	/*! Optional path of an initial eIM configuration in BER
	 *  (JSON: initial_eim_cfg_path).  NULL when not configured. */
	char *initial_eim_cfg_path;

	/*! Perform an eUICC memory reset instead of polling
	 *  (JSON: euicc_memory_reset). */
	bool euicc_memory_reset;

	/*! Stop after a single eUICC package (JSON: one_euicc_pkg_only).
	 *  Debug aid; mirrors the CLI's -1. */
	bool one_euicc_pkg_only;

	/*! Rotating-file log sink settings (JSON: the "log" object), handed to
	 *  ipa_log_file_sink_init() by ipa_run().  path is NULL when no log file
	 *  was configured, which means "keep the default stderr sink".  The two
	 *  size limits default to IPA_DEFAULT_LOG_MAX_* (see log_sink.h); an
	 *  explicit max_size_bytes of 0 disables rotation. */
	struct {
		char *path;
		size_t max_size_bytes;
		unsigned int max_files;
	} log;
};

/*! Fill a run configuration with the built-in defaults (the same values the
 *  CLI uses when no flag is given).  Any previously held strings are leaked,
 *  so call this only on a zeroed or fresh struct.
 *  \param[out] rcfg run configuration to initialize. */
void ipa_run_config_defaults(struct ipa_run_config *rcfg);

/*! Parse a JSON configuration from memory.
 *  \param[in] json JSON text (need not be NUL-terminated).
 *  \param[in] json_len length of json in bytes.
 *  \returns newly allocated run configuration (free with
 *           ipa_run_config_free()), or NULL when the text is not valid JSON,
 *           carries an unknown key, or a value has the wrong type or range. */
struct ipa_run_config *ipa_config_json_parse(const char *json, size_t json_len);

/*! Load and parse a JSON configuration file.
 *  \param[in] path path of the configuration file.
 *  \returns newly allocated run configuration (free with
 *           ipa_run_config_free()), or NULL when the file is missing,
 *           unreadable or invalid.  The reason is logged. */
struct ipa_run_config *ipa_config_json_load(const char *path);

/*! Release a run configuration and everything it owns (NULL-safe).
 *  \param[in] rcfg run configuration to free. */
void ipa_run_config_free(struct ipa_run_config *rcfg);

/*! Run the IPAd from an already parsed configuration: load the nvstate file,
 *  create and initialize the context, then either apply the initial eIM
 *  configuration, perform the memory reset, or enter the poll loop; on exit
 *  the nvstate is written back.  Blocks until the poll cycle ends or
 *  ipa_run_stop() is called.
 *  The context keeps a pointer to rcfg->cfg (it does not copy it), so rcfg
 *  must stay alive for the whole call -- and cfg.tac may be updated in place
 *  while it runs, as struct ipa_config documents.
 *  \param[in] rcfg run configuration (not freed).
 *  \returns 0 on success, negative on error. */
int ipa_run(struct ipa_run_config *rcfg);

/*! Convenience wrapper: load a JSON configuration file and run it.  This is
 *  the entry point the Android daemon and the APK call.
 *  \param[in] json_path path of the configuration file.
 *  \returns 0 on success, negative on error. */
int ipa_run_from_config(const char *json_path);

/*! Ask a running ipa_run() / ipa_run_from_config() to leave the poll loop at
 *  the next opportunity.  Async-signal-safe, so it may be called from a
 *  signal handler (the CLI wires it to SIGUSR1) or from another thread. */
void ipa_run_stop(void);
