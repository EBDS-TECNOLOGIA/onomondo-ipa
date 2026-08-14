/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Tests for ipa_run()'s single-run guard (ANDROID_PORT_PLAN.md, Phase 4).
 *
 * An Android foreground service that gets restarted, or a daemon started
 * twice, must not end up with two poll loops sharing the stop flag and the
 * nvstate file.  Rather than racing two threads and hoping to observe the
 * collision, the stub transport below re-enters ipa_run() from inside the
 * first call -- which is the same reentrancy, made deterministic.
 *
 * The stubs stand in for the PC/SC and curl backends that main.c links; they
 * only have to be enough for ipa_init() to fail fast.
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/scard.h>
#include <onomondo/ipa/http.h>
#include <onomondo/ipa/config_json.h>

static struct ipa_run_config *the_cfg;
static int reentrant_rc;
static bool reenter;

/* ipa_init() calls this first; it is therefore a hook that runs *inside*
 * ipa_run().  Returning NULL makes ipa_init() fail, which is all we need. */
void *ipa_scard_init(unsigned int reader_num)
{
	(void)reader_num;

	if (reenter) {
		reenter = false;
		reentrant_rc = ipa_run(the_cfg);
	}
	return NULL;
}

int ipa_scard_reset(void *c) { (void)c; return -1; }
int ipa_scard_atr(void *c, struct ipa_buf *a) { (void)c; (void)a; return -1; }
int ipa_scard_transceive(void *c, struct ipa_buf *r, const struct ipa_buf *s)
{ (void)c; (void)r; (void)s; return -1; }
int ipa_scard_free(void *c) { (void)c; return 0; }
bool ipa_scard_manages_channel(void *c) { (void)c; return false; }

void *ipa_http_init(const char *ca, bool nv) { (void)ca; (void)nv; return (void *)1; }
struct ipa_buf *ipa_http_req(void *c, const struct ipa_buf *r, const char *u)
{ (void)c; (void)r; (void)u; return NULL; }
struct ipa_buf *ipa_http_req_with_ct(void *c, const struct ipa_buf *r, const char *u, const char *t)
{ (void)c; (void)r; (void)u; (void)t; return NULL; }
void ipa_http_close(void *c) { (void)c; }
void ipa_http_free(void *c) { (void)c; }
void ipa_http_set_timeouts(void *c, long a, long b) { (void)c; (void)a; (void)b; }
int ipa_http_set_ca_cert_der(void *c, const uint8_t *d, size_t l) { (void)c; (void)d; (void)l; return 0; }
int ipa_http_set_ca_pk_spki(void *c, const uint8_t *s, size_t l) { (void)c; (void)s; (void)l; return 0; }
int ipa_http_set_client_cert_der(void *c, const uint8_t *d, size_t l, ipa_tls_sign_fn f, void *a)
{ (void)c; (void)d; (void)l; (void)f; (void)a; return 0; }

int main(int argc, char **argv)
{
	int rc;

	{
		static const char json[] = "{\"nvstate_path\": \"/dev/null\"}";

		the_cfg = ipa_config_json_parse(json, strlen(json));
	}
	if (!the_cfg) {
		printf("built without jansson -- JSON configuration unavailable, skipping\n");
		return 0;
	}

	/* A second run started while the first is in flight is refused ... */
	printf("reentrant_run_test\n");
	reenter = true;
	reentrant_rc = 0;
	rc = ipa_run(the_cfg);
	assert(rc < 0); /* ipa_init failed, as the stub arranged */
	assert(reentrant_rc == -EBUSY);

	/* ... and the guard is released afterwards, so the next run proceeds
	 * normally rather than being refused forever. */
	printf("sequential_run_test\n");
	reenter = false;
	rc = ipa_run(the_cfg);
	assert(rc != -EBUSY);

	/* A NULL configuration is rejected without touching the guard. */
	assert(ipa_run(NULL) == -EINVAL);
	rc = ipa_run(the_cfg);
	assert(rc != -EBUSY);

	ipa_run_config_free(the_cfg);
	printf("run_guard_test: all tests passed\n");
	return 0;
}
