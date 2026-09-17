/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Tests for the JSON configuration keys added for the OpenWrt port: log.level, log.subsys_levels and the
 * platform object.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/config_json.h>

static struct ipa_run_config *parse(const char *json)
{
	return ipa_config_json_parse(json, strlen(json));
}

static void defaults_test(void)
{
	struct ipa_run_config *rcfg = parse("{}");
	unsigned int i;

	printf("defaults_test\n");
	assert(rcfg->log.level == -1);
	for (i = 0; i < _NUM_LOG_SUBSYS; i++)
		assert(rcfg->log.subsys_level[i] == -1);
	assert(!rcfg->has_platform);
	ipa_run_config_free(rcfg);
}

static void levels_test(void)
{
	struct ipa_run_config *rcfg;

	printf("levels_test\n");
	rcfg = parse("{\"log\": {\"level\": \"error\", \"subsys_levels\": {\"es10x\": \"debug\", \"HTTP\": \"INFO\","
		     " \"_why\": \"case does not matter\"}}}");
	assert(rcfg);
	assert(rcfg->log.level == LERROR);
	assert(rcfg->log.subsys_level[SES10X] == LDEBUG);
	assert(rcfg->log.subsys_level[SHTTP] == LINFO);
	assert(rcfg->log.subsys_level[SMAIN] == -1);
	/* A level alone does not open a log file. */
	assert(rcfg->log.path == NULL);
	ipa_run_config_free(rcfg);
}

static void platform_test(void)
{
	struct ipa_run_config *rcfg;

	printf("platform_test\n");
	/* Whatever the platform keeps in there is not the parser's business. */
	rcfg = parse("{\"platform\": {\"wan_interfaces\": [\"wan_modem\"], \"anything\": {\"nested\": 1}}}");
	assert(rcfg);
	assert(rcfg->has_platform);
	ipa_run_config_free(rcfg);
}

static void rejection_test(void)
{
	static const char *const bad[] = {
		"{\"log\": {\"level\": \"verbose\"}}",
		"{\"log\": {\"level\": 2}}",
		"{\"log\": {\"subsys_levels\": {\"NOSUCH\": \"debug\"}}}",
		"{\"log\": {\"subsys_levels\": {\"HTTP\": \"loud\"}}}",
		"{\"log\": {\"subsys_levels\": [\"HTTP\"]}}",
		"{\"platform\": [\"not\", \"an\", \"object\"]}",
		"{\"platform\": \"wan\"}",
	};
	unsigned int i;

	printf("rejection_test\n");
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		assert(parse(bad[i]) == NULL);
}

int main(void)
{
	struct ipa_run_config *probe = parse("{}");

	if (!probe) {
		printf("built without jansson -- JSON configuration unavailable, skipping\n");
		return 0;
	}
	ipa_run_config_free(probe);

	defaults_test();
	levels_test();
	platform_test();
	rejection_test();

	printf("config_ext_test: all tests passed\n");
	return 0;
}
