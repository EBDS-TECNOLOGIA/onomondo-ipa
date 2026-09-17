/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Client side of http_tls_test.sh: runs the real HTTP client (http.c with whichever http_tls implementation the
 * build has) against a local HTTPS server.
 *
 * usage: http_tls_test URL STEP...
 *   cabundle:FILE   ipa_http_init() with this CA bundle (must come first, if at all)
 *   noverif         ipa_http_init() with verification off (must come first, if at all)
 *   ca-der:FILE     ipa_http_set_ca_cert_der()
 *   ca-spki:FILE    ipa_http_set_ca_pk_spki()
 *   req             one request; prints OK when the server's answer arrived, FAIL otherwise
 * The printed words, one per "req", are what the script compares.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/http.h>
#include <onomondo/ipa/log.h>

static int load(const char *path, uint8_t *buf, size_t size)
{
	FILE *fp = fopen(path, "rb");
	size_t len;

	if (!fp) {
		perror(path);
		exit(2);
	}
	len = fread(buf, 1, size, fp);
	fclose(fp);
	return (int)len;
}

int main(int argc, char **argv)
{
	const char *url;
	const char *cabundle = NULL;
	bool no_verif = false;
	uint8_t buf[4096];
	struct ipa_buf *req;
	struct ipa_buf *res;
	void *http;
	int first = 2;
	int i;
	int len;

	if (argc < 3)
		return 2;
	url = argv[1];

	/* Only the log lines of failures are of interest; the script shows them when a case goes wrong. */
	ipa_log_set_level_all(LERROR);

	if (strncmp(argv[2], "cabundle:", 9) == 0) {
		cabundle = argv[2] + 9;
		first = 3;
	} else if (strcmp(argv[2], "noverif") == 0) {
		no_verif = true;
		first = 3;
	}

	http = ipa_http_init(cabundle, no_verif);
	ipa_http_set_timeouts(http, 5, 10);
	req = ipa_buf_alloc_data(4, (uint8_t *)"ping");

	for (i = first; i < argc; i++) {
		if (strncmp(argv[i], "ca-der:", 7) == 0) {
			len = load(argv[i] + 7, buf, sizeof(buf));
			if (ipa_http_set_ca_cert_der(http, buf, len) < 0)
				printf("BADANCHOR ");
		} else if (strncmp(argv[i], "ca-spki:", 8) == 0) {
			len = load(argv[i] + 8, buf, sizeof(buf));
			if (ipa_http_set_ca_pk_spki(http, buf, len) < 0)
				printf("BADANCHOR ");
		} else if (strcmp(argv[i], "req") == 0) {
			res = ipa_http_req(http, req, url);
			printf("%s ", res && res->len == 4 && memcmp(res->data, "pong", 4) == 0 ? "OK" : "FAIL");
			IPA_FREE(res);
		} else {
			fprintf(stderr, "unknown step %s\n", argv[i]);
			return 2;
		}
	}
	printf("\n");

	IPA_FREE(req);
	ipa_http_free(http);
	return 0;
}
