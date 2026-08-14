/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * ANDROID_PORT_PLAN.md, Phase 2 -- JSON configuration.
 *
 * Design notes
 * ------------
 * One JSON key per CLI flag, all flat except the "log" object that Phase 3's
 * rotating-file sink will consume.  Parsing is deliberately strict: an
 * unknown key, a value of the wrong JSON type, or an out-of-range number is
 * a hard error rather than something silently ignored.  On an unattended IoT
 * device a typo in the configuration would otherwise run the IPAd on a
 * default the operator never intended, which is far worse than refusing to
 * start with a log line naming the offending key.
 *
 * Absent keys are not errors: they keep the default from
 * ipa_run_config_defaults(), which is exactly what the CLI uses when the
 * matching flag is not given.
 *
 * Compile-time switch: IPA_HAVE_JANSSON (set from CMake when jansson is
 * detected).  Without it the loader fails loudly -- see the stub at the end.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <onomondo/ipa/mem.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/config_json.h>
#include "fileio.h"

/* strdup() through the project allocator, so allocations stay balanced in the
 * MEM_EMIT_DEBUG accounting that IPA_ALLOC_N / IPA_FREE maintain. */
static char *cfg_strdup(const char *str)
{
	size_t len = strlen(str) + 1;
	char *out = IPA_ALLOC_N_ZERO(len);

	memcpy(out, str, len);
	return out;
}

void ipa_run_config_defaults(struct ipa_run_config *rcfg)
{
	memset(rcfg, 0, sizeof(*rcfg));

	rcfg->cfg.reader_num = IPA_DEFAULT_READER_NUMBER;
	rcfg->cfg.euicc_channel = IPA_DEFAULT_CHANNEL_NUMBER;
	rcfg->cfg.esipa_req_retries = IPA_DEFAULT_ESIPA_REQ_RETRIES;
	rcfg->cfg.esipa_binding = IPA_ESIPA_BINDING_ASN1;
	ipa_binary_from_hexstr(rcfg->cfg.tac, sizeof(rcfg->cfg.tac), IPA_DEFAULT_TAC);

	rcfg->nvstate_path = cfg_strdup(IPA_DEFAULT_NVSTATE_PATH);
}

void ipa_run_config_free(struct ipa_run_config *rcfg)
{
	if (!rcfg)
		return;

	/* struct ipa_config does not own its strings; the storage they point
	 * at belongs to us (see the ownership note in config_json.h). */
	IPA_FREE(rcfg->cfg.preferred_eim_id);
	IPA_FREE((char *)rcfg->cfg.eim_cabundle);
	IPA_FREE(rcfg->nvstate_path);
	IPA_FREE(rcfg->initial_eim_cfg_path);
	IPA_FREE(rcfg->log.path);
	IPA_FREE(rcfg);
}

#ifdef IPA_HAVE_JANSSON

#include <jansson.h>

/* Every key the parser accepts.  Used both to reject unknown keys and to
 * keep this list, the CLI flags and the plan's mapping table in one view. */
/* Upper bounds for the rotating-file sink.  These are sanity limits, not
 * spec numbers: a log file larger than 1 GiB or more than 1000 generations
 * on an IoT device is a configuration mistake, and bounding them here keeps
 * the narrowing to size_t / unsigned int well defined on 32-bit targets. */
#define LOG_MAX_SIZE_LIMIT (1024LL * 1024 * 1024)
#define LOG_MAX_FILES_LIMIT 1000

static const char *const known_keys[] = {
	"tac",			/* -t */
	"preferred_eim_id",	/* -e */
	"reader_num",		/* -r */
	"euicc_channel",	/* -c */
	"initial_eim_cfg_path",	/* -f */
	"euicc_memory_reset",	/* -m */
	"nvstate_path",		/* -n */
	"esipa_req_retries",	/* -y */
	"eim_cabundle",		/* -C */
	"eim_disable_ssl",	/* -S */
	"eim_disable_ssl_verif",/* -I */
	"iot_euicc_emu_enabled",/* -E */
	"one_euicc_pkg_only",	/* -1 */
	"refresh_flag",		/* -R */
	"esipa_binding",	/* (no flag; ASN.1 by default) */
	"log",			/* (no flag; Phase 3 rotating-file sink) */
};

/* JSON has no comment syntax, but an operator-edited configuration file on a
 * device badly wants one.  Any key starting with an underscore is therefore
 * ignored, which is the usual convention ("_comment": "..."). */
static bool key_is_comment(const char *key)
{
	return key[0] == '_';
}

static bool key_is_known(const char *key)
{
	unsigned int i;

	if (key_is_comment(key))
		return true;

	for (i = 0; i < IPA_ARRAY_SIZE(known_keys); i++) {
		if (strcmp(key, known_keys[i]) == 0)
			return true;
	}
	return false;
}

/* Each getter leaves *out untouched (i.e. at its default) when the key is
 * absent, and returns -EINVAL after logging when the key is present but
 * unusable.  path is the human readable location for the log line, e.g.
 * "log." for members of the log object. */

static int get_str(json_t *obj, const char *path, const char *key, char **out)
{
	json_t *val = json_object_get(obj, key);

	if (!val)
		return 0;
	if (!json_is_string(val)) {
		IPA_LOGP(SMAIN, LERROR, "config: \"%s%s\" must be a string\n", path, key);
		return -EINVAL;
	}

	IPA_FREE(*out);
	*out = cfg_strdup(json_string_value(val));
	return 0;
}

static int get_bool(json_t *obj, const char *path, const char *key, bool *out)
{
	json_t *val = json_object_get(obj, key);

	if (!val)
		return 0;
	if (!json_is_boolean(val)) {
		IPA_LOGP(SMAIN, LERROR, "config: \"%s%s\" must be true or false\n", path, key);
		return -EINVAL;
	}

	*out = json_is_true(val);
	return 0;
}

/* Read a non-negative integer, rejecting anything above max.  jansson gives
 * us a json_int_t, so the range check also catches negative values and
 * values that would wrap when narrowed. */
static int get_uint(json_t *obj, const char *path, const char *key, json_int_t max, json_int_t *out)
{
	json_t *val = json_object_get(obj, key);
	json_int_t num;

	if (!val)
		return 0;
	if (!json_is_integer(val)) {
		IPA_LOGP(SMAIN, LERROR, "config: \"%s%s\" must be an integer\n", path, key);
		return -EINVAL;
	}

	num = json_integer_value(val);
	if (num < 0 || num > max) {
		IPA_LOGP(SMAIN, LERROR, "config: \"%s%s\" out of range (0..%lld), got %lld\n", path, key,
			 (long long)max, (long long)num);
		return -EINVAL;
	}

	*out = num;
	return 0;
}

/* TAC: a hex string of exactly IPA_LEN_TAC bytes.  ipa_binary_from_hexstr is
 * lenient (it substitutes 0xff for junk and stops at the buffer end), so the
 * string is validated here before handing it over. */
static int get_tac(json_t *obj, uint8_t *tac)
{
	json_t *val = json_object_get(obj, "tac");
	const char *str;
	size_t i;

	if (!val)
		return 0;
	if (!json_is_string(val)) {
		IPA_LOGP(SMAIN, LERROR, "config: \"tac\" must be a string\n");
		return -EINVAL;
	}

	str = json_string_value(val);
	if (strlen(str) != IPA_LEN_TAC * 2) {
		IPA_LOGP(SMAIN, LERROR, "config: \"tac\" must be %d hex digits, got %zu\n", IPA_LEN_TAC * 2,
			 strlen(str));
		return -EINVAL;
	}
	for (i = 0; str[i]; i++) {
		if (!isxdigit((unsigned char)str[i])) {
			IPA_LOGP(SMAIN, LERROR, "config: \"tac\" is not a hex string: %s\n", str);
			return -EINVAL;
		}
	}

	ipa_binary_from_hexstr(tac, IPA_LEN_TAC, str);
	return 0;
}

static int get_binding(json_t *obj, enum ipa_esipa_binding *out)
{
	json_t *val = json_object_get(obj, "esipa_binding");
	const char *str;

	if (!val)
		return 0;
	if (!json_is_string(val)) {
		IPA_LOGP(SMAIN, LERROR, "config: \"esipa_binding\" must be a string\n");
		return -EINVAL;
	}

	str = json_string_value(val);
	if (strcmp(str, "asn1") == 0)
		*out = IPA_ESIPA_BINDING_ASN1;
	else if (strcmp(str, "json") == 0)
		*out = IPA_ESIPA_BINDING_JSON;
	else {
		IPA_LOGP(SMAIN, LERROR, "config: \"esipa_binding\" must be \"asn1\" or \"json\", got \"%s\"\n", str);
		return -EINVAL;
	}

	return 0;
}

/* The "log" object (Phase 3).  Parsed and validated now so a daemon does not
 * discover a bad log configuration only once the sink is wired up. */
static int get_log(json_t *obj, struct ipa_run_config *rcfg)
{
	static const char *const log_keys[] = { "path", "max_size_bytes", "max_files" };
	json_t *log = json_object_get(obj, "log");
	json_int_t num;
	const char *key;
	json_t *unused;
	unsigned int i;
	bool known;

	if (!log)
		return 0;
	if (!json_is_object(log)) {
		IPA_LOGP(SMAIN, LERROR, "config: \"log\" must be an object\n");
		return -EINVAL;
	}

	json_object_foreach(log, key, unused) {
		known = key_is_comment(key);
		for (i = 0; i < IPA_ARRAY_SIZE(log_keys); i++) {
			if (strcmp(key, log_keys[i]) == 0)
				known = true;
		}
		if (!known) {
			IPA_LOGP(SMAIN, LERROR, "config: unknown key \"log.%s\"\n", key);
			return -EINVAL;
		}
	}

	if (get_str(log, "log.", "path", &rcfg->log.path) < 0)
		return -EINVAL;

	num = (json_int_t)rcfg->log.max_size_bytes;
	if (get_uint(log, "log.", "max_size_bytes", (json_int_t)LOG_MAX_SIZE_LIMIT, &num) < 0)
		return -EINVAL;
	rcfg->log.max_size_bytes = (size_t)num;

	num = rcfg->log.max_files;
	if (get_uint(log, "log.", "max_files", (json_int_t)LOG_MAX_FILES_LIMIT, &num) < 0)
		return -EINVAL;
	rcfg->log.max_files = (unsigned int)num;

	return 0;
}

struct ipa_run_config *ipa_config_json_parse(const char *json, size_t json_len)
{
	struct ipa_run_config *rcfg;
	json_error_t err;
	json_t *obj;
	json_int_t num;
	const char *key;
	json_t *unused;

	obj = json_loadb(json, json_len, 0, &err);
	if (!obj) {
		IPA_LOGP(SMAIN, LERROR, "config: not valid JSON (line %d: %s)\n", err.line, err.text);
		return NULL;
	}
	if (!json_is_object(obj)) {
		IPA_LOGP(SMAIN, LERROR, "config: top level must be a JSON object\n");
		json_decref(obj);
		return NULL;
	}

	/* Reject typos before applying anything, so a bad file never results
	 * in a half-applied configuration. */
	json_object_foreach(obj, key, unused) {
		if (!key_is_known(key)) {
			IPA_LOGP(SMAIN, LERROR, "config: unknown key \"%s\"\n", key);
			json_decref(obj);
			return NULL;
		}
	}

	rcfg = IPA_ALLOC_ZERO(struct ipa_run_config);
	ipa_run_config_defaults(rcfg);

	if (get_tac(obj, rcfg->cfg.tac) < 0)
		goto err;
	if (get_str(obj, "", "preferred_eim_id", &rcfg->cfg.preferred_eim_id) < 0)
		goto err;
	if (get_str(obj, "", "nvstate_path", &rcfg->nvstate_path) < 0)
		goto err;
	if (get_str(obj, "", "initial_eim_cfg_path", &rcfg->initial_eim_cfg_path) < 0)
		goto err;
	/* cfg.eim_cabundle is const char * but the storage is ours to free. */
	if (get_str(obj, "", "eim_cabundle", (char **)&rcfg->cfg.eim_cabundle) < 0)
		goto err;

	if (get_bool(obj, "", "euicc_memory_reset", &rcfg->euicc_memory_reset) < 0)
		goto err;
	if (get_bool(obj, "", "one_euicc_pkg_only", &rcfg->one_euicc_pkg_only) < 0)
		goto err;
	if (get_bool(obj, "", "eim_disable_ssl", &rcfg->cfg.eim_disable_ssl) < 0)
		goto err;
	if (get_bool(obj, "", "eim_disable_ssl_verif", &rcfg->cfg.eim_disable_ssl_verif) < 0)
		goto err;
	if (get_bool(obj, "", "iot_euicc_emu_enabled", &rcfg->cfg.iot_euicc_emu_enabled) < 0)
		goto err;
	if (get_bool(obj, "", "refresh_flag", &rcfg->cfg.refresh_flag) < 0)
		goto err;

	num = rcfg->cfg.reader_num;
	if (get_uint(obj, "", "reader_num", UINT_MAX, &num) < 0)
		goto err;
	rcfg->cfg.reader_num = (unsigned int)num;

	num = rcfg->cfg.euicc_channel;
	if (get_uint(obj, "", "euicc_channel", IPA_MAX_CHANNEL_NUMBER, &num) < 0)
		goto err;
	rcfg->cfg.euicc_channel = (uint8_t)num;

	num = rcfg->cfg.esipa_req_retries;
	if (get_uint(obj, "", "esipa_req_retries", UINT_MAX, &num) < 0)
		goto err;
	rcfg->cfg.esipa_req_retries = (unsigned int)num;

	if (get_binding(obj, &rcfg->cfg.esipa_binding) < 0)
		goto err;
	if (get_log(obj, rcfg) < 0)
		goto err;

	json_decref(obj);
	return rcfg;

err:
	json_decref(obj);
	ipa_run_config_free(rcfg);
	return NULL;
}

struct ipa_run_config *ipa_config_json_load(const char *path)
{
	struct ipa_run_config *rcfg;
	struct ipa_buf *file;

	if (!path) {
		IPA_LOGP(SMAIN, LERROR, "config: no configuration file path given\n");
		return NULL;
	}

	file = ipa_file_load(path, 0);
	if (!file) {
		IPA_LOGP(SMAIN, LERROR, "config: cannot read configuration file %s: %s\n", path, strerror(errno));
		return NULL;
	}

	rcfg = ipa_config_json_parse((const char *)file->data, file->len);
	IPA_FREE(file);

	if (!rcfg) {
		IPA_LOGP(SMAIN, LERROR, "config: %s rejected, not starting\n", path);
		return NULL;
	}

	IPA_LOGP(SMAIN, LINFO, "config: loaded %s\n", path);
	return rcfg;
}

#else /* !IPA_HAVE_JANSSON --------------------------------------------- */

/* Without jansson there is no way to read the configuration file.  Fail
 * loudly rather than starting on defaults the operator never asked for. */

struct ipa_run_config *ipa_config_json_parse(const char *json, size_t json_len)
{
	(void)json;
	(void)json_len;
	IPA_LOGP(SMAIN, LERROR, "config: built without jansson, JSON configuration is unavailable\n");
	return NULL;
}

struct ipa_run_config *ipa_config_json_load(const char *path)
{
	(void)path;
	IPA_LOGP(SMAIN, LERROR, "config: built without jansson, JSON configuration is unavailable "
				"(install libjansson-dev and rebuild)\n");
	return NULL;
}

#endif /* IPA_HAVE_JANSSON */
