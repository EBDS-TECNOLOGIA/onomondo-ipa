/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * ANDROID_PORT_PLAN.md, Phase 2 -- config-file driven entry point.
 *
 * This is the same run sequence src/ipa/main.c performs for the Linux CLI,
 * expressed once so the Android daemon (Phase 4) and the APK (Phase 5), which
 * have no command line, share it: load the nvstate, create and initialize the
 * context, then either apply an initial eIM configuration, perform an eUICC
 * memory reset, or enter the poll loop; finally write the nvstate back.
 *
 * Not covered here, on purpose: the ES10b one-shot triggers (-i/-F/-b/-X/-x/
 * -G/-D) and the profile-installation consent callback (-a).  The triggers
 * are device-policy decisions a daemon makes through the ipa_* API in
 * ipad.h at the moment the device signals them, not something to encode in a
 * configuration file; the callback is deprecated (github issue #5) and cannot
 * be expressed as JSON anyway.
 */

#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <onomondo/ipa/mem.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/ipad.h>
#include <onomondo/ipa/config_json.h>
#include <onomondo/ipa/log_sink.h>
#include "fileio.h"

/* Set from ipa_run_stop(), which may run in a signal handler or in another
 * thread, hence volatile sig_atomic_t rather than bool. */
static volatile sig_atomic_t run_stop;

/* One run at a time.  An Android foreground service that gets restarted, or a
 * daemon started twice, would otherwise have two poll loops sharing run_stop
 * and the same nvstate file, with the second overwriting the first's state on
 * exit. */
static pthread_mutex_t run_lock = PTHREAD_MUTEX_INITIALIZER;
static bool run_active;

void ipa_run_stop(void)
{
	run_stop = 1;
}

static void log_config(const struct ipa_run_config *rcfg)
{
	IPA_LOGP(SMAIN, LINFO, "nvstate path: %s\n", rcfg->nvstate_path);
	IPA_LOGP(SMAIN, LINFO, "preferred_eim_id = %s\n",
		 rcfg->cfg.preferred_eim_id ? rcfg->cfg.preferred_eim_id : "(first configured eIM)");
	IPA_LOGP(SMAIN, LINFO, "reader_num = %u\n", rcfg->cfg.reader_num);
	IPA_LOGP(SMAIN, LINFO, "euicc_channel = %u\n", rcfg->cfg.euicc_channel);
	if (rcfg->cfg.eim_cabundle)
		IPA_LOGP(SMAIN, LINFO, "eim_cabundle = %s\n", rcfg->cfg.eim_cabundle);
	IPA_LOGP(SMAIN, LINFO, "eim_disable_ssl = %d\n", rcfg->cfg.eim_disable_ssl);
	IPA_LOGP(SMAIN, LINFO, "eim_disable_ssl_verif = %d\n", rcfg->cfg.eim_disable_ssl_verif);
	IPA_LOGP(SMAIN, LINFO, "tac = %s\n", ipa_hexdump(rcfg->cfg.tac, sizeof(rcfg->cfg.tac)));
	IPA_LOGP(SMAIN, LINFO, "iot_euicc_emu_enabled = %u\n", rcfg->cfg.iot_euicc_emu_enabled);
	IPA_LOGP(SMAIN, LINFO, "esipa_req_retries = %u\n", rcfg->cfg.esipa_req_retries);
	IPA_LOGP(SMAIN, LINFO, "esipa_binding = %s\n",
		 rcfg->cfg.esipa_binding == IPA_ESIPA_BINDING_JSON ? "json" : "asn1");
	IPA_LOGP(SMAIN, LINFO, "refresh_flag = %u\n", rcfg->cfg.refresh_flag);
	IPA_LOGP(SMAIN, LINFO, "poll_interval = %u %s (%u s; 0 = single cycle)\n", rcfg->poll_interval,
		 rcfg->poll_interval_unit == IPA_POLL_INTERVAL_MINUTES ? "minutes" : "seconds",
		 ipa_run_config_poll_seconds(rcfg));
}

/* The poll loop, identical in behaviour to the CLI's.  Returns 0 when the
 * cycle ended normally, negative on an unrecoverable error. */
static int poll_loop(struct ipa_context *ctx, bool one_euicc_pkg_only)
{
	int rc;

	while (!run_stop) {
		IPA_LOGP(SMAIN, LINFO, "-----------------------------8<-----------------------------\n");
		rc = ipa_poll(ctx);

		switch (rc) {
		case IPA_POLL_AGAIN_WHEN_ONLINE:
			/* A profile change may have dropped IP connectivity.  A real
			 * daemon would wait for the bearer to come back before polling
			 * again; here we assume connectivity is restored by then. */
			IPA_LOGP(SMAIN, LINFO, "poll cycle continues normally (profile change)\n");
			break;
		case IPA_POLL_AGAIN:
			if (one_euicc_pkg_only) {
				IPA_LOGP(SMAIN, LINFO, "forcefully stopping poll cycle upon user decision!\n");
				return 0;
			}
			IPA_LOGP(SMAIN, LINFO, "poll cycle continues normally\n");
			break;
		case IPA_POLL_AGAIN_LATER:
			/* Nothing left to do for now. */
			IPA_LOGP(SMAIN, LINFO, "poll cycle ends normally\n");
			return 0;
		default:
			IPA_LOGP(SMAIN, LERROR, "poll cycle ends due to error (%d)\n", rc);
			return -EINVAL;
		}
	}

	IPA_LOGP(SMAIN, LINFO, "poll cycle stopped on request\n");
	return 0;
}

int ipa_run(struct ipa_run_config *rcfg)
{
	struct ipa_context *ctx = NULL;
	struct ipa_buf *nvstate_load = NULL;
	struct ipa_buf *nvstate_save = NULL;
	struct ipa_buf *eim_cfg = NULL;
	bool log_file_open = false;
	int rc;

	if (!rcfg)
		return -EINVAL;

	pthread_mutex_lock(&run_lock);
	if (run_active) {
		pthread_mutex_unlock(&run_lock);
		IPA_LOGP(SMAIN, LERROR, "IPAd is already running; refusing to start a second poll loop\n");
		return -EBUSY;
	}
	run_active = true;
	pthread_mutex_unlock(&run_lock);

	run_stop = 0;

	/* Switch to the rotating-file sink before anything is logged, so the
	 * configuration banner lands in the file too.  A log file we cannot open
	 * is worth complaining about loudly, but it is diagnostics rather than
	 * function: the IPAd carries on with the stderr sink instead of refusing
	 * to run the device's provisioning. */
	if (rcfg->log.path) {
		rc = ipa_log_file_sink_init(rcfg->log.path, rcfg->log.max_size_bytes, rcfg->log.max_files);
		if (rc < 0) {
			log_file_open = false;
			IPA_LOGP(SMAIN, LERROR, "cannot open log file %s: %s -- logging to stderr\n",
				 rcfg->log.path, strerror(-rc));
		} else {
			log_file_open = true;
		}
	}

	/* Session marker: in a rotating log file this is what tells you where one
	 * run ends and the next begins.  The CLI's own "IPAd!" banner stays on
	 * stdout for the interactive user. */
	IPA_LOGP(SMAIN, LINFO, "IPAd starting\n");
	log_config(rcfg);

	if (rcfg->cfg.eim_cabundle && access(rcfg->cfg.eim_cabundle, R_OK) < 0) {
		IPA_LOGP(SMAIN, LERROR, "error accessing CA bundle %s: %s\n", rcfg->cfg.eim_cabundle,
			 strerror(errno));
		rc = -EINVAL;
		goto close_log;
	}

	/* A missing nvstate file is normal on first start -- the context then
	 * creates a fresh state. */
	nvstate_load = ipa_file_load(rcfg->nvstate_path, 0);
	if (nvstate_load)
		IPA_LOGP(SMAIN, LINFO, "loaded nvstate from file %s, size: %zu\n", rcfg->nvstate_path,
			 nvstate_load->len);
	else
		IPA_LOGP(SMAIN, LINFO, "unable to load nvstate from file %s -- a new nvstate will be created.\n",
			 rcfg->nvstate_path);

	ctx = ipa_new_ctx(&rcfg->cfg, nvstate_load);
	if (!ctx) {
		IPA_LOGP(SMAIN, LERROR, "cannot create context!\n");
		rc = -EINVAL;
		goto leave;
	}

	IPA_LOGP(SMAIN, LINFO, "-----------------------------8<-----------------------------\n");
	rc = ipa_init(ctx);
	if (rc < 0) {
		IPA_LOGP(SMAIN, LERROR, "IPAd initialization failed!\n");
		rc = -EINVAL;
		goto leave;
	}

	if (rcfg->initial_eim_cfg_path) {
		/* The BER blob gets one spare byte, matching the CLI loader. */
		eim_cfg = ipa_file_load(rcfg->initial_eim_cfg_path, 1);
		if (!eim_cfg) {
			IPA_LOGP(SMAIN, LERROR, "failed to load initial eIM configuration from %s\n",
				 rcfg->initial_eim_cfg_path);
			rc = -EINVAL;
			goto leave;
		}
		IPA_LOGP(SMAIN, LINFO, "loaded BER data from file %s, size: %zu\n", rcfg->initial_eim_cfg_path,
			 eim_cfg->len);
		rc = ipa_add_init_eim_cfg(ctx, eim_cfg);
		IPA_FREE(eim_cfg);
	} else if (rcfg->euicc_memory_reset) {
		rc = ipa_euicc_mem_rst(ctx, true, true, true, true, true);
	} else {
		IPA_LOGP(SMAIN, LINFO, "-----------------------------8<-----------------------------\n");
		rc = eim_init(ctx);
		if (rc < 0) {
			IPA_LOGP(SMAIN, LERROR, "eIM initialization failed!\n");
			rc = -EINVAL;
			goto leave;
		}
		rc = poll_loop(ctx, rcfg->one_euicc_pkg_only);
	}

leave:
	IPA_LOGP(SMAIN, LINFO, "-----------------------------8<-----------------------------\n");
	nvstate_save = ipa_free_ctx(ctx);
	if (nvstate_save) {
		if (ipa_file_save(rcfg->nvstate_path, nvstate_save) < 0)
			IPA_LOGP(SMAIN, LERROR, "unable to save nvstate to file %s!\n", rcfg->nvstate_path);
		else
			IPA_LOGP(SMAIN, LINFO, "saved nvstate to file %s, size: %zu\n", rcfg->nvstate_path,
				 nvstate_save->data_len);
	}
	IPA_FREE(nvstate_load);
	IPA_FREE(nvstate_save);

close_log:
	if (log_file_open)
		ipa_log_file_sink_free();

	pthread_mutex_lock(&run_lock);
	run_active = false;
	pthread_mutex_unlock(&run_lock);

	return rc;
}

int ipa_run_from_config(const char *json_path)
{
	struct ipa_run_config *rcfg;
	int rc;

	rcfg = ipa_config_json_load(json_path);
	if (!rcfg)
		return -EINVAL;

	rc = ipa_run(rcfg);
	ipa_run_config_free(rcfg);
	return rc;
}
