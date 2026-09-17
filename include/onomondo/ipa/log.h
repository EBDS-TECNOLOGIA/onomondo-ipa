/*
 * Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

/*! Redirect the log to a sink of the caller's choosing, discarding any sinks
 *  already installed.  The default, with no sink installed, writes to stderr,
 *  which is what the Linux CLI uses; the Android daemon installs the
 *  rotating-file sink and the APK the ring-buffer sink (see
 *  onomondo/ipa/log_sink.h).
 *  \param[in] sink sink to install, or NULL to restore the stderr default. */
void ipa_log_set_sink(ipa_log_sink_cb sink);

/*! Add a sink, keeping the ones already installed.  Every record goes to every
 *  installed sink, in the order they were added; with none installed it goes to
 *  stderr.  This is what lets the APK keep its live ring-buffer view while
 *  ipa_run() also writes the rotating log file.  Adding the same sink twice is
 *  a no-op.
 *  \param[in] sink sink to add.
 *  \returns 0 on success, -EINVAL if sink is NULL, -ENOSPC if the sink table
 *           is full. */
int ipa_log_add_sink(ipa_log_sink_cb sink);

/*! Remove a sink previously added with ipa_log_add_sink() or installed with
 *  ipa_log_set_sink(); removing one that is not installed is a no-op.  Once the
 *  last sink is gone, records go to stderr again.
 *  \param[in] sink sink to remove. */
void ipa_log_del_sink(ipa_log_sink_cb sink);

/*! Print the source file and line that produced each log line, as "(file.c:123)".
 *
 *  Off by default, for two reasons. The unit tests compare log output byte for byte against golden files, and
 *  __FILE__ holds whatever path the compiler was handed -- an absolute one in this project -- so the location
 *  is only reproducible because just the last path component is printed.
 *
 *  \param[in] enable true to include the source location, false to leave it out. */
void ipa_log_set_print_source(bool enable);

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

/* Runtime control of what gets logged. Each subsystem carries its own maximum level (LDEBUG for all of them
 * unless changed, i.e. nothing is filtered out) and its own enable flag; a log line is printed only when the
 * subsystem is enabled and the line's level is at or below the subsystem's maximum. */

void ipa_log_set_level(enum log_subsys subsys, enum log_level level);
void ipa_log_set_level_all(enum log_level level);
enum log_level ipa_log_get_level(enum log_subsys subsys);

void ipa_log_set_subsys_enabled(enum log_subsys subsys, bool enable);
bool ipa_log_subsys_enabled(enum log_subsys subsys);

bool ipa_log_check(enum log_subsys subsys, enum log_level level);

/* Name lookups. These exist so that a caller that has to turn a user-supplied string into a subsystem or a level
 * -- the command line of the sample application, a configuration file -- does not need a second copy of the name
 * tables, which would drift the first time a subsystem is added. */

int ipa_log_subsys_by_name(const char *name);
int ipa_log_level_by_name(const char *name);
const char *ipa_log_subsys_name(enum log_subsys subsys);
const char *ipa_log_level_name(enum log_level level);
