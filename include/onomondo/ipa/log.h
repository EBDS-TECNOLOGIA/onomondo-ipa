/*
 * Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

/*! macro to print a log line.
 *  \param[in] subsys log subsystem identifier.
 *  \param[in] level log level identifier.
 *  \param[in] fmt formtstring.
 *  \param[in] args formatstring arguments. */
#define IPA_LOGP(subsys, level, fmt, args...) \
	ipa_logp(subsys, level, __FILE__, __LINE__, fmt, ## args)

void ipa_logp(uint32_t subsys, uint32_t level, const char *file, int line,
	      const char *format, ...)
    __attribute__((format(printf, 5, 6)));

/*! Log sink: receives one fully formatted log record -- subsystem/level
 *  prefix, message and its trailing newline -- in a single call.  Records are
 *  never split across calls, so a sink may treat each call as one line.
 *  \param[in] line the formatted record; NUL-terminated, but len is
 *             authoritative.
 *  \param[in] len length of the record in bytes, excluding the NUL. */
typedef void (*ipa_log_sink_cb)(const char *line, size_t len);

/*! Redirect the log to a sink of the caller's choosing.  The default sink
 *  writes to stderr, which is what the Linux CLI uses; the Android daemon
 *  installs the rotating-file sink and the APK the ring-buffer sink (see
 *  onomondo/ipa/log_sink.h).
 *  \param[in] sink sink to install, or NULL to restore the stderr default. */
void ipa_log_set_sink(ipa_log_sink_cb sink);

enum log_subsys {
	SMAIN,
	SHTTP,
	SSCARD,
	SIPA,
	SES10X,
	SES10B,
	SEUICC,
	SESIPA,
	_NUM_LOG_SUBSYS
};

enum log_level {
	LERROR,
	LINFO,
	LDEBUG,
	_NUM_LOG_LEVEL
};
