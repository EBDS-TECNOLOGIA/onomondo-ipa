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
#include <errno.h>
#include <pthread.h>
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

/* Sinks are a set, not a single slot: the APK needs the rotating-file sink and
 * the ring-buffer sink at the same time (persistent log + live view), and
 * whichever one installed itself second must not silently displace the first.
 * With the set empty, records go to stderr -- the Linux CLI's behaviour, byte
 * for byte.  Four is more than any front-end has needed; a fifth is refused
 * loudly rather than dropped silently. */
#define LOG_MAX_SINKS 4

static ipa_log_sink_cb sinks[LOG_MAX_SINKS];
static unsigned int num_sinks;
static pthread_mutex_t sink_lock = PTHREAD_MUTEX_INITIALIZER;

int ipa_log_add_sink(ipa_log_sink_cb sink)
{
	unsigned int i;
	int rc = 0;

	if (!sink)
		return -EINVAL;

	pthread_mutex_lock(&sink_lock);
	for (i = 0; i < num_sinks; i++) {
		if (sinks[i] == sink)
			goto out; /* already installed; adding twice is a no-op */
	}
	if (num_sinks == LOG_MAX_SINKS) {
		rc = -ENOSPC;
		goto out;
	}
	sinks[num_sinks++] = sink;
out:
	pthread_mutex_unlock(&sink_lock);
	return rc;
}

void ipa_log_del_sink(ipa_log_sink_cb sink)
{
	unsigned int i;

	pthread_mutex_lock(&sink_lock);
	for (i = 0; i < num_sinks; i++) {
		if (sinks[i] != sink)
			continue;
		memmove(&sinks[i], &sinks[i + 1], (num_sinks - i - 1) * sizeof(sinks[0]));
		num_sinks--;
		break;
	}
	pthread_mutex_unlock(&sink_lock);
}

void ipa_log_set_sink(ipa_log_sink_cb sink)
{
	pthread_mutex_lock(&sink_lock);
	num_sinks = 0;
	if (sink)
		sinks[num_sinks++] = sink;
	pthread_mutex_unlock(&sink_lock);
}

/* Dispatch a finished record.  The sink table is copied under the lock and the
 * sinks are called outside it, so a sink is free to take its own lock (both of
 * ours do) without ordering against this one, and a sink that logs cannot
 * deadlock against a concurrent add/del. */
static void log_emit(const char *line, size_t len)
{
	ipa_log_sink_cb local[LOG_MAX_SINKS];
	unsigned int i, n;

	pthread_mutex_lock(&sink_lock);
	n = num_sinks;
	memcpy(local, sinks, n * sizeof(local[0]));
	pthread_mutex_unlock(&sink_lock);

	if (!n) {
		stderr_sink(line, len);
		return;
	}
	for (i = 0; i < n; i++)
		local[i](line, len);
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

	log_emit(buf, total);

	if (buf != stack_buf)
		free(buf);
}
