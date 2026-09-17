/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

struct ipa_context;

/* ===========================================================================
 * Observing ipa_run() (OPENWRT_PORT_ANALYSIS.md, sections 4 and 7)
 * ===========================================================================
 *
 * ipa_run() owns the context for the length of a run, so a front end has no
 * other way to see what happens inside it. The observer is how the OpenWrt
 * daemon fills its status file (EID, eIM, outcome of each poll) and how it
 * holds the poll loop after a profile change until the WAN link is back:
 * the observer runs on ipa_run()'s thread, and ipa_run() carries on only
 * once it returns.
 *
 * Kept apart from config_json.h so that header stays identical to the one on
 * the Android port.
 */

enum ipa_run_event {
	/*! ipa_init() succeeded; the context can be queried (ipa_get_ctx_info()). rc is 0. */
	IPA_RUN_EV_INITIALIZED,
	/*! eim_init() succeeded; the eIM is now known too. rc is 0. */
	IPA_RUN_EV_EIM_READY,
	/*! ipa_poll() returned; rc is its return value (enum ipa_poll_rc or negative). The context stays valid
	 *  for the duration of the call. */
	IPA_RUN_EV_POLLED,
};

/*! Observer callback.
 *  \param[in] ctx the context ipa_run() is working with; do not keep it past the call.
 *  \param[in] ev what happened.
 *  \param[in] rc event specific result, see enum ipa_run_event.
 *  \param[in] priv the pointer given to ipa_run_set_observer(). */
typedef void (*ipa_run_observer_cb)(struct ipa_context *ctx, enum ipa_run_event ev, int rc, void *priv);

/*! Install the observer for subsequent ipa_run() calls, replacing any previous one.
 *  Not synchronized: set it before starting a run.
 *  \param[in] cb callback, or NULL to remove it.
 *  \param[in] priv passed to every call of cb. */
void ipa_run_set_observer(ipa_run_observer_cb cb, void *priv);
