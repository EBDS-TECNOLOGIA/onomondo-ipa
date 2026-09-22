/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * The ipa_scard_* functions the core calls, forwarded to the transport the front end selected with
 * ipa_scard_set_transport() -- see onomondo/ipa/scard_transport.h for the URIs.
 *
 * The PC/SC implementation in scard.c is compiled with its ipa_scard_* names mapped to ipa_scard_pcsc_* (a
 * compile definition, see linux_daemon.cmake), so that file stays as it is and keeps merging with the other
 * ports.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <assert.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/scard.h>
#include <onomondo/ipa/scard_transport.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/mem.h>
#include "scard_at.h"

#ifdef IPA_TRANSPORT_PCSC
/* The PC/SC backend, renamed by the build; declared here rather than in scard.h, which the rename also covers. */
void *ipa_scard_pcsc_init(unsigned int reader_num);
int ipa_scard_pcsc_reset(void *scard_ctx);
int ipa_scard_pcsc_atr(void *scard_ctx, struct ipa_buf *atr);
int ipa_scard_pcsc_transceive(void *scard_ctx, struct ipa_buf *res, const struct ipa_buf *req);
int ipa_scard_pcsc_free(void *scard_ctx);
#endif

struct scard_backend {
	const char *scheme;
	void *(*init)(const char *arg, unsigned int reader_num);
	int (*reset)(void *ctx);
	int (*atr)(void *ctx, struct ipa_buf *atr);
	int (*transceive)(void *ctx, struct ipa_buf *res, const struct ipa_buf *req);
	int (*free)(void *ctx);
};

#ifdef IPA_TRANSPORT_PCSC
/* The PC/SC backend takes a reader number, which the URI may carry (pcsc:2) and ipa_config.reader_num otherwise. */
static void *pcsc_init(const char *arg, unsigned int reader_num)
{
	if (arg && *arg) {
		char *end;
		unsigned long num = strtoul(arg, &end, 10);

		if (*end || num > UINT_MAX) {
			IPA_LOGP(SSCARD, LERROR, "PC/SC transport: \"%s\" is not a reader number\n", arg);
			return NULL;
		}
		reader_num = (unsigned int)num;
	}
	return ipa_scard_pcsc_init(reader_num);
}
#endif

static const struct scard_backend backends[] = {
#ifdef IPA_TRANSPORT_PCSC
	{ "pcsc", pcsc_init, ipa_scard_pcsc_reset, ipa_scard_pcsc_atr, ipa_scard_pcsc_transceive,
	  ipa_scard_pcsc_free },
#endif
	{ "at", ipa_scard_at_init, ipa_scard_at_reset, ipa_scard_at_atr, ipa_scard_at_transceive,
	  ipa_scard_at_free },
};

/* What ipa_scard_set_transport() selected: the backend and the part of the URI after the scheme. The first entry
 * is the default: PC/SC where it is built, the AT transport otherwise. */
static const struct scard_backend *selected = &backends[0];
static char transport_uri[256] =
#ifdef IPA_TRANSPORT_PCSC
	"pcsc";
#else
	"at";
#endif
static char transport_arg[256];

/* The core holds one void * per card; this is what it holds, so that the right backend is called back. */
struct scard_handle {
	const struct scard_backend *backend;
	void *ctx;
};

int ipa_scard_set_transport(const char *uri)
{
	const char *colon;
	size_t scheme_len;
	unsigned int i;

	/* No URI: the first backend this build has, which is PC/SC where it is built and the AT transport
	 * otherwise. */
	if (!uri || !*uri)
		uri = backends[0].scheme;

	colon = strchr(uri, ':');
	scheme_len = colon ? (size_t)(colon - uri) : strlen(uri);

	for (i = 0; i < IPA_ARRAY_SIZE(backends); i++) {
		if (strlen(backends[i].scheme) != scheme_len || strncmp(uri, backends[i].scheme, scheme_len) != 0)
			continue;

		selected = &backends[i];
		snprintf(transport_uri, sizeof(transport_uri), "%s", uri);
		snprintf(transport_arg, sizeof(transport_arg), "%s", colon ? colon + 1 : "");
		/* Debug: the front ends print the transport with the rest of the configuration, and the daemon
		 * selects it again on every poll cycle. */
		IPA_LOGP(SSCARD, LDEBUG, "eUICC transport: %s\n", transport_uri);
		return 0;
	}

	IPA_LOGP(SSCARD, LERROR, "unknown eUICC transport \"%s\"\n", uri);
	for (i = 0; i < IPA_ARRAY_SIZE(backends); i++)
		IPA_LOGP(SSCARD, LERROR, "known transport: %s:\n", backends[i].scheme);
	return -EINVAL;
}

const char *ipa_scard_get_transport(void)
{
	return transport_uri;
}

const char *ipa_scard_transport_schemes(void)
{
	static char schemes[64];
	unsigned int i;

	if (!schemes[0]) {
		for (i = 0; i < IPA_ARRAY_SIZE(backends); i++) {
			if (i)
				strncat(schemes, ", ", sizeof(schemes) - strlen(schemes) - 1);
			strncat(schemes, backends[i].scheme, sizeof(schemes) - strlen(schemes) - 1);
		}
	}
	return schemes;
}

void *ipa_scard_init(unsigned int reader_num)
{
	struct scard_handle *handle;
	void *ctx = selected->init(transport_arg, reader_num);

	if (!ctx)
		return NULL;

	handle = IPA_ALLOC_ZERO(struct scard_handle);
	handle->backend = selected;
	handle->ctx = ctx;
	return handle;
}

int ipa_scard_reset(void *scard_ctx)
{
	struct scard_handle *handle = scard_ctx;

	return handle ? handle->backend->reset(handle->ctx) : -EINVAL;
}

int ipa_scard_atr(void *scard_ctx, struct ipa_buf *atr)
{
	struct scard_handle *handle = scard_ctx;

	return handle ? handle->backend->atr(handle->ctx, atr) : -EINVAL;
}

int ipa_scard_transceive(void *scard_ctx, struct ipa_buf *res, const struct ipa_buf *req)
{
	struct scard_handle *handle = scard_ctx;

	return handle ? handle->backend->transceive(handle->ctx, res, req) : -EINVAL;
}

int ipa_scard_free(void *scard_ctx)
{
	struct scard_handle *handle = scard_ctx;
	int rc;

	if (!handle)
		return 0;
	rc = handle->backend->free(handle->ctx);
	IPA_FREE(handle);
	return rc;
}
