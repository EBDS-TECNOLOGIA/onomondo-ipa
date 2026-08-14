/*
 * Copyright (c) 2025 Onomondo ApS. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Author: Philipp Maier <pmaier@sysmocom.de> / sysmocom - s.f.m.c. GmbH
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/utils.h>

uint32_t ipa_log_mask = 0xffffffff;

/* TODO: how to modify log levels at runtime or from getopt */
static uint32_t subsys_lvl[_NUM_LOG_SUBSYS] = {
	[SMAIN] = LDEBUG,
	[SHTTP] = LDEBUG,
	[SSCARD] = LDEBUG,
	[SIPA] = LDEBUG,
	[SES10X] = LDEBUG,
	[SES10B] = LDEBUG,
	[SEUICC] = LDEBUG,
	[SESIPA] = LDEBUG,
};

static const char *subsys_str[_NUM_LOG_SUBSYS] = {
	[SMAIN] = "MAIN",
	[SHTTP] = "HTTP",
	[SSCARD] = "SCARD",
	[SIPA] = "IPA",
	[SES10X] = "ES10x",
	[SES10B] = "ES10b",
	[SEUICC] = "eUICC",
	[SESIPA] = "ESIPA",
};

static const char *level_str[_NUM_LOG_LEVEL] = {
	[LERROR] = "ERROR",
	[LINFO] = "INFO",
	[LDEBUG] = "DEBUG",
};

/* Default sink, and the behaviour the Linux CLI has always had: write the
 * record straight to stderr.  One fwrite per record rather than the previous
 * two fprintf calls, which also stops concurrent writers from interleaving a
 * prefix with somebody else's message. */
static void stderr_sink(const char *line, size_t len)
{
	fwrite(line, 1, len, stderr);
}

static ipa_log_sink_cb log_sink = stderr_sink;

void ipa_log_set_sink(ipa_log_sink_cb sink)
{
	log_sink = sink ? sink : stderr_sink;
}

/* Records are usually well under this; the stack buffer just avoids a malloc
 * on the common path.  Anything longer (APDU hexdumps, mostly) is formatted
 * again into a heap buffer rather than being truncated. */
#define LOG_STACK_BUF 512

/*! print a log line (called by IPA_LOGP, do not call directly).
 *  \param[in] subsys log subsystem identifier.
 *  \param[in] level log level identifier.
 *  \param[in] file source file name.
 *  \param[in] line source file line.
 *  \param[in] format format string (followed by arguments). */
void ipa_logp(uint32_t subsys, uint32_t level, const char *file, int line, const char *format, ...)
{
	char stack_buf[LOG_STACK_BUF];
	char *buf = stack_buf;
	int prefix_len;
	int msg_len;
	size_t total;
	va_list ap;

	if (!(ipa_log_mask & (1 << subsys)))
		return;

	assert(subsys < IPA_ARRAY_SIZE(subsys_lvl));

	if (level > subsys_lvl[subsys])
		return;

	/* TODO: print file and line, but make it an optional feature that
	 * can be selected via commandline option. The reason for this is that
	 * the unit-tests may compare the log output against .err files and
	 * even on minor changes we would constantly upset the unit-tests. */

	/* The prefix is a fixed 18 bytes, so it always fits. */
	prefix_len = snprintf(stack_buf, sizeof(stack_buf), "%8s %8s ", subsys_str[subsys], level_str[level]);
	if (prefix_len < 0)
		return;

	va_start(ap, format);
	msg_len = vsnprintf(stack_buf + prefix_len, sizeof(stack_buf) - prefix_len, format, ap);
	va_end(ap);
	if (msg_len < 0)
		return;

	/* vsnprintf returns what it *would* have written, so this detects the
	 * truncation without a second guess at the size. */
	total = (size_t)prefix_len + (size_t)msg_len;
	if (total >= sizeof(stack_buf)) {
		buf = malloc(total + 1);
		if (!buf) {
			/* Out of memory: emit the truncated record rather than
			 * losing the line entirely. */
			buf = stack_buf;
			total = sizeof(stack_buf) - 1;
		} else {
			memcpy(buf, stack_buf, (size_t)prefix_len);
			va_start(ap, format);
			vsnprintf(buf + prefix_len, total - prefix_len + 1, format, ap);
			va_end(ap);
		}
	}

	log_sink(buf, total);

	if (buf != stack_buf)
		free(buf);
}
