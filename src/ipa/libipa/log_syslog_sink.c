/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * syslog log sink, see onomondo/ipa/log_syslog.h.
 */

#include <string.h>
#include <syslog.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/log_syslog.h>

/* ipa_logp() starts every record with "%8s %8s ": the subsystem and the level, each right-aligned in eight
 * columns. The level is read back from there rather than passed alongside the record, which keeps the sink
 * interface the one the other sinks share. */
#define PREFIX_FIELD 8
#define PREFIX_LEN (2 * PREFIX_FIELD + 2)

/* Longest record forwarded; syslog implementations truncate far below this anyway. */
#define MAX_RECORD 1024

static int priority_of(const char *level, size_t len)
{
	/* Skip the padding in front of the right-aligned name. */
	while (len && *level == ' ') {
		level++;
		len--;
	}

	if (len == strlen(ipa_log_level_name(LERROR)) && memcmp(level, ipa_log_level_name(LERROR), len) == 0)
		return LOG_ERR;
	if (len == strlen(ipa_log_level_name(LDEBUG)) && memcmp(level, ipa_log_level_name(LDEBUG), len) == 0)
		return LOG_DEBUG;
	return LOG_INFO;
}

static void syslog_sink_write(const char *line, size_t len)
{
	const char *subsys = line;
	size_t subsys_len = PREFIX_FIELD;
	const char *msg;
	int priority;

	if (len < PREFIX_LEN || line[PREFIX_FIELD] != ' ' || line[PREFIX_LEN - 1] != ' ') {
		/* Not a record ipa_logp() formatted; pass it on as it is. */
		priority = LOG_INFO;
		subsys_len = 0;
		msg = line;
	} else {
		priority = priority_of(line + PREFIX_FIELD + 1, PREFIX_FIELD);
		msg = line + PREFIX_LEN;
		len -= PREFIX_LEN;
		while (subsys_len && *subsys == ' ') {
			subsys++;
			subsys_len--;
		}
	}

	/* syslog appends its own line end, so drop the record's. */
	while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
		len--;
	if (len > MAX_RECORD)
		len = MAX_RECORD;

	if (subsys_len)
		syslog(priority, "%.*s: %.*s", (int)subsys_len, subsys, (int)len, msg);
	else
		syslog(priority, "%.*s", (int)len, msg);
}

int ipa_log_syslog_sink_init(const char *ident)
{
	openlog(ident, LOG_PID, LOG_DAEMON);
	return ipa_log_add_sink(syslog_sink_write);
}

void ipa_log_syslog_sink_free(void)
{
	ipa_log_del_sink(syslog_sink_write);
	closelog();
}
