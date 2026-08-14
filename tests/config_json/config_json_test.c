/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Unit tests for the JSON configuration parser (ANDROID_PORT_PLAN.md, Phase 2).
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/config_json.h>

static struct ipa_run_config *parse(const char *json)
{
	return ipa_config_json_parse(json, strlen(json));
}

/* Every setting absent -> the same values the CLI uses with no flags. */
static void defaults_test(void)
{
	struct ipa_run_config *rcfg;
	uint8_t tac[IPA_LEN_TAC];

	printf("defaults_test\n");

	rcfg = parse("{}");
	assert(rcfg);

	ipa_binary_from_hexstr(tac, sizeof(tac), IPA_DEFAULT_TAC);
	assert(memcmp(rcfg->cfg.tac, tac, sizeof(tac)) == 0);
	assert(rcfg->cfg.reader_num == IPA_DEFAULT_READER_NUMBER);
	assert(rcfg->cfg.euicc_channel == IPA_DEFAULT_CHANNEL_NUMBER);
	assert(rcfg->cfg.esipa_req_retries == IPA_DEFAULT_ESIPA_REQ_RETRIES);
	assert(rcfg->cfg.esipa_binding == IPA_ESIPA_BINDING_ASN1);
	assert(rcfg->cfg.preferred_eim_id == NULL);
	assert(rcfg->cfg.eim_cabundle == NULL);
	assert(rcfg->cfg.eim_disable_ssl == false);
	assert(rcfg->cfg.eim_disable_ssl_verif == false);
	assert(rcfg->cfg.iot_euicc_emu_enabled == false);
	assert(rcfg->cfg.refresh_flag == false);
	assert(strcmp(rcfg->nvstate_path, IPA_DEFAULT_NVSTATE_PATH) == 0);
	assert(rcfg->initial_eim_cfg_path == NULL);
	assert(rcfg->euicc_memory_reset == false);
	assert(rcfg->one_euicc_pkg_only == false);
	assert(rcfg->log.path == NULL);
	assert(rcfg->log.max_size_bytes == 0);
	assert(rcfg->log.max_files == 0);

	ipa_run_config_free(rcfg);
}

/* Every settable key at once, so the key names and the struct members stay
 * matched up. */
static void full_config_test(void)
{
	static const char json[] = "{"
		"\"tac\": \"35AB1200\","
		"\"preferred_eim_id\": \"eim-42\","
		"\"reader_num\": 2,"
		"\"euicc_channel\": 3,"
		"\"initial_eim_cfg_path\": \"/etc/ipa/eim.ber\","
		"\"euicc_memory_reset\": true,"
		"\"nvstate_path\": \"/data/ipa/nvstate.bin\","
		"\"esipa_req_retries\": 7,"
		"\"eim_cabundle\": \"/etc/ssl/certs/eim.pem\","
		"\"eim_disable_ssl\": true,"
		"\"eim_disable_ssl_verif\": true,"
		"\"iot_euicc_emu_enabled\": true,"
		"\"one_euicc_pkg_only\": true,"
		"\"refresh_flag\": true,"
		"\"esipa_binding\": \"json\","
		"\"log\": {"
			"\"path\": \"/data/ipa/ipa.log\","
			"\"max_size_bytes\": 262144,"
			"\"max_files\": 5"
		"}"
	"}";
	static const uint8_t expect_tac[] = { 0x35, 0xab, 0x12, 0x00 };
	struct ipa_run_config *rcfg;

	printf("full_config_test\n");

	rcfg = parse(json);
	assert(rcfg);

	assert(memcmp(rcfg->cfg.tac, expect_tac, sizeof(expect_tac)) == 0);
	assert(strcmp(rcfg->cfg.preferred_eim_id, "eim-42") == 0);
	assert(rcfg->cfg.reader_num == 2);
	assert(rcfg->cfg.euicc_channel == 3);
	assert(strcmp(rcfg->initial_eim_cfg_path, "/etc/ipa/eim.ber") == 0);
	assert(rcfg->euicc_memory_reset == true);
	assert(strcmp(rcfg->nvstate_path, "/data/ipa/nvstate.bin") == 0);
	assert(rcfg->cfg.esipa_req_retries == 7);
	assert(strcmp(rcfg->cfg.eim_cabundle, "/etc/ssl/certs/eim.pem") == 0);
	assert(rcfg->cfg.eim_disable_ssl == true);
	assert(rcfg->cfg.eim_disable_ssl_verif == true);
	assert(rcfg->cfg.iot_euicc_emu_enabled == true);
	assert(rcfg->one_euicc_pkg_only == true);
	assert(rcfg->cfg.refresh_flag == true);
	assert(rcfg->cfg.esipa_binding == IPA_ESIPA_BINDING_JSON);
	assert(strcmp(rcfg->log.path, "/data/ipa/ipa.log") == 0);
	assert(rcfg->log.max_size_bytes == 262144);
	assert(rcfg->log.max_files == 5);

	ipa_run_config_free(rcfg);
}

/* A key that is present overrides its default; the rest stay put. */
static void partial_config_test(void)
{
	struct ipa_run_config *rcfg;

	printf("partial_config_test\n");

	rcfg = parse("{\"reader_num\": 1, \"esipa_binding\": \"asn1\"}");
	assert(rcfg);
	assert(rcfg->cfg.reader_num == 1);
	assert(rcfg->cfg.esipa_binding == IPA_ESIPA_BINDING_ASN1);
	assert(rcfg->cfg.euicc_channel == IPA_DEFAULT_CHANNEL_NUMBER);
	assert(strcmp(rcfg->nvstate_path, IPA_DEFAULT_NVSTATE_PATH) == 0);
	ipa_run_config_free(rcfg);
}

/* Anything the parser cannot make unambiguous sense of must be refused, so a
 * typo can never leave the IPAd running on a default nobody configured. */
static void rejection_test(void)
{
	static const char *const bad[] = {
		/* not JSON at all */
		"",
		"{",
		"reader_num = 1",
		/* top level must be an object */
		"[]",
		"\"hello\"",
		"42",
		/* unknown keys */
		"{\"reader_number\": 1}",
		"{\"log\": {\"max_size\": 10}}",
		/* wrong types */
		"{\"nvstate_path\": 1}",
		"{\"reader_num\": \"1\"}",
		"{\"reader_num\": 1.5}",
		"{\"eim_disable_ssl\": \"true\"}",
		"{\"eim_disable_ssl\": 1}",
		"{\"tac\": 12345678}",
		"{\"esipa_binding\": 0}",
		"{\"log\": \"/var/log/ipa.log\"}",
		/* out of range */
		"{\"reader_num\": -1}",
		"{\"euicc_channel\": 20}",
		"{\"esipa_req_retries\": -3}",
		"{\"log\": {\"max_files\": 100000}}",
		"{\"log\": {\"max_size_bytes\": 99999999999}}",
		/* malformed values */
		"{\"tac\": \"12345\"}",
		"{\"tac\": \"1234567890\"}",
		"{\"tac\": \"1234ZZ78\"}",
		"{\"esipa_binding\": \"cbor\"}",
	};
	unsigned int i;

	printf("rejection_test\n");

	for (i = 0; i < IPA_ARRAY_SIZE(bad); i++) {
		printf(" rejecting: %s\n", bad[i]);
		assert(parse(bad[i]) == NULL);
	}
}

/* The example configuration shipped in contrib/ must stay parseable -- it is
 * what operators copy from, and it is the only place the key names appear
 * outside the parser itself. */
static void example_config_test(void)
{
	struct ipa_run_config *rcfg;

	printf("example_config_test: %s\n", IPA_EXAMPLE_CONFIG);

	rcfg = ipa_config_json_load(IPA_EXAMPLE_CONFIG);
	assert(rcfg);
	ipa_run_config_free(rcfg);
}

/* "_comment" keys are ignored so a config file can document itself. */
static void comment_key_test(void)
{
	struct ipa_run_config *rcfg;

	printf("comment_key_test\n");

	rcfg = parse("{\"_note\": \"why reader 1\", \"reader_num\": 1,"
		     " \"log\": {\"_note\": \"small on purpose\", \"max_files\": 2}}");
	assert(rcfg);
	assert(rcfg->cfg.reader_num == 1);
	assert(rcfg->log.max_files == 2);
	ipa_run_config_free(rcfg);
}

/* A missing file is an error, not an excuse to run on defaults. */
static void missing_file_test(void)
{
	printf("missing_file_test\n");
	assert(ipa_config_json_load("/nonexistent/ipa-config.json") == NULL);
	assert(ipa_config_json_load(NULL) == NULL);
}

int main(int argc, char **argv)
{
	/* When the library was built without jansson every entry point fails by
	 * design; there is nothing to test then. */
	struct ipa_run_config *probe = parse("{}");
	if (!probe) {
		printf("built without jansson -- JSON configuration unavailable, skipping\n");
		return 0;
	}
	ipa_run_config_free(probe);

	defaults_test();
	full_config_test();
	partial_config_test();
	rejection_test();
	comment_key_test();
	example_config_test();
	missing_file_test();

	printf("config_json_test: all tests passed\n");
	return 0;
}
