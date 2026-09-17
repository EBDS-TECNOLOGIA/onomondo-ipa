/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

/* ===========================================================================
 * syslog sink (OPENWRT_PORT_ANALYSIS.md, section 7.4)
 * ===========================================================================
 *
 * Forwards every log record to syslog(3).  On OpenWrt that is logd, so the
 * records show up in logread, in LuCI's system log and on a remote syslog
 * server if one is configured; on a systemd host they land in the journal.
 *
 * The record's level becomes the syslog priority (LERROR -> LOG_ERR,
 * LINFO -> LOG_INFO, LDEBUG -> LOG_DEBUG) and is dropped from the text, since
 * syslog carries it already.  The subsystem stays, as "ES10x: ...".
 *
 * Kept apart from log_sink.h so that header stays identical to the one on
 * the Android port.
 */

/*! Open syslog and add it to the installed log sinks.
 *  \param[in] ident identifier syslog prefixes each record with, e.g. "ipad";
 *             must stay valid until ipa_log_syslog_sink_free().
 *  \returns 0 on success, negative on error (see ipa_log_add_sink()). */
int ipa_log_syslog_sink_init(const char *ident);

/*! Remove the syslog sink and close syslog.  Safe to call when the sink is
 *  not installed. */
void ipa_log_syslog_sink_free(void);
