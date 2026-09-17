/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Tests for the syslog log sink. openlog(), syslog() and closelog() are defined here, which makes the sink in the
 * (static) libipa call these instead of the C library's, so the test sees exactly what would reach syslog.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <syslog.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/log_syslog.h>

static char last_ident[32];
static int last_priority = -1;
static char last_msg[2048];
static unsigned int calls;
static bool is_open;

void openlog(const char *ident, int option, int facility)
{
	snprintf(last_ident, sizeof(last_ident), "%s", ident);
	assert(option & LOG_PID);
	assert(facility == LOG_DAEMON);
	is_open = true;
}

void syslog(int priority, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(last_msg, sizeof(last_msg), format, ap);
	va_end(ap);
	last_priority = priority;
	calls++;
}

void closelog(void)
{
	is_open = false;
}

static void expect(int priority, const char *msg)
{
	if (last_priority != priority || strcmp(last_msg, msg) != 0) {
		printf("expected <%d> \"%s\", got <%d> \"%s\"\n", priority, msg, last_priority, last_msg);
		assert(0);
	}
}

int main(void)
{
	char big[3000];

	printf("init_test\n");
	assert(ipa_log_syslog_sink_init("ipad") == 0);
	assert(is_open);
	assert(strcmp(last_ident, "ipad") == 0);

	printf("level_mapping_test\n");
	IPA_LOGP(SES10X, LERROR, "card said %04x\n", 0x6985);
	expect(LOG_ERR, "ES10x: card said 6985");
	IPA_LOGP(SMAIN, LINFO, "hello\n");
	expect(LOG_INFO, "MAIN: hello");
	IPA_LOGP(SESIPA, LDEBUG, "detail\n");
	expect(LOG_DEBUG, "ESIPA: detail");

	printf("no_newline_test\n");
	IPA_LOGP(SHTTP, LINFO, "no line end");
	expect(LOG_INFO, "HTTP: no line end");

	printf("source_location_test\n");
	ipa_log_set_print_source(true);
	IPA_LOGP(SEUICC, LERROR, "with source\n");
	ipa_log_set_print_source(false);
	assert(last_priority == LOG_ERR);
	assert(strncmp(last_msg, "eUICC: (log_syslog_test.c:", strlen("eUICC: (log_syslog_test.c:")) == 0);

	printf("long_record_test\n");
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	IPA_LOGP(SMAIN, LINFO, "%s\n", big);
	assert(last_priority == LOG_INFO);
	assert(strlen(last_msg) == strlen("MAIN: ") + 1024);

	printf("filtered_test\n");
	calls = 0;
	ipa_log_set_level(SMAIN, LINFO);
	IPA_LOGP(SMAIN, LDEBUG, "dropped\n");
	assert(calls == 0);
	ipa_log_set_level(SMAIN, LDEBUG);

	printf("free_test\n");
	ipa_log_syslog_sink_free();
	assert(!is_open);
	calls = 0;
	fprintf(stderr, "(the next line is expected on stderr) ");
	IPA_LOGP(SMAIN, LINFO, "back on stderr\n");
	assert(calls == 0);

	printf("log_syslog_test: all tests passed\n");
	return 0;
}
