/*
 * Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Author: Philipp Maier <pmaier@sysmocom.de> / sysmocom - s.f.m.c. GmbH
 * Author: Harald Welte <hwelte@sysmocom.de> / sysmocom - s.f.m.c. GmbH
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <curl/curl.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/http.h>
#include <onomondo/ipa/http_hdr.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/mem.h>
/* The eUICC-provisioned TLS trust anchor is installed through the TLS library libcurl runs on, see http_tls.h. */
#include "http_tls.h"

/* -------------------------------------------------------------------------
 * HTTP client context
 * ------------------------------------------------------------------------- */

/* Default TCP connect timeout: bounds only the connection phase. */
#define IPA_HTTP_CONNECT_TIMEOUT_S 10
/* Default whole-request timeout.  The old value (5s) covered the entire
 * transfer, which is too short for a BoundProfilePackage download; use a
 * generous default and expose it via ipa_http_set_timeouts(). */
#define IPA_HTTP_TOTAL_TIMEOUT_S 300

struct http_ctx {
	bool initialized;
	const char *cabundle;  /* path-based CA bundle (legacy / fallback) */
	bool no_verif;
	bool tls_backend_ok; /* libcurl runs on the TLS library http_tls was built for */
	long connect_timeout_s; /* TCP connect phase timeout (CURLOPT_CONNECTTIMEOUT) */
	long total_timeout_s;   /* whole-request timeout (CURLOPT_TIMEOUT) */
	CURL *curl;
	/* eUICC-provisioned TLS credentials (trustedCertificateTls, trustedEimPkTls). */
	struct http_tls *tls;
};

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/*! Initialize HTTP client.
 *  \param[in] cabundle path to a CA bundle (used when no DER cert is set).
 *  \param[in] no_verif skip SSL certificate verification (insecure).
 *  \returns pointer to newly allocated HTTP client context. */
/* curl_global_init()/curl_global_cleanup() are documented as per-*program*
 * (not per-handle) and are not thread-safe.  A single context made them look
 * per-context, but with more than one HTTP context live at once (e.g. eIM +
 * SM-DP+ for direct download) a per-context cleanup would tear the global state
 * down while another context is still using it.  Refcount them so the global
 * init runs once on the first context and the global cleanup runs once after
 * the last one is freed.  This assumes the single-threaded ipa_poll() model
 * used throughout the code base (the counter is not atomic). */
static unsigned int curl_global_refcnt;

void *ipa_http_init(const char *cabundle, bool no_verif)
{
	struct http_ctx *ctx = IPA_ALLOC(struct http_ctx);
	assert(ctx);
	memset(ctx, 0, sizeof(*ctx));

	if (curl_global_refcnt++ == 0)
		curl_global_init(CURL_GLOBAL_DEFAULT);
	ctx->initialized = true;
	ctx->cabundle = cabundle;
	ctx->no_verif = no_verif;
	ctx->connect_timeout_s = IPA_HTTP_CONNECT_TIMEOUT_S;
	ctx->total_timeout_s = IPA_HTTP_TOTAL_TIMEOUT_S;
	ctx->tls = http_tls_alloc();
	ctx->tls_backend_ok = http_tls_curl_matches();

	/* The eUICC-provisioned trust anchor (SGP.32 trustedPublicKeyDataTls) is installed through
	 * CURLOPT_SSL_CTX_FUNCTION, whose argument is the TLS library's own configuration object, so this build
	 * and libcurl have to agree on the library. */
	if (!ctx->tls_backend_ok) {
		const curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
		IPA_LOGP(SHTTP, LERROR,
			 "libcurl is built against %s, but this IPAd for %s. TLS credentials stored "
			 "in the eUICC cannot be used; only -C <cabundle> will work. "
			 "Rebuild with a matching -DIPA_HTTP_TLS.\n",
			 (vi && vi->ssl_version) ? vi->ssl_version : "an unknown TLS library",
			 http_tls_backend_name());
	}

	IPA_LOGP(SHTTP, LINFO, "HTTP client initialized.\n");

	return ctx;
}

/*! Set the CA certificate for TLS server verification from a DER blob
 *  (eUICC trustedCertificateTls).  When set, this supersedes the cabundle
 *  path from ipa_http_init().
 *  \param[in] der   DER-encoded X.509 certificate bytes.
 *  \param[in] len   byte length of der.
 *  \returns 0 on success, -EINVAL if the DER cannot be parsed. */
int ipa_http_set_ca_cert_der(void *http_ctx, const uint8_t *der, size_t len)
{
	struct http_ctx *ctx = http_ctx;

	assert(ctx);
	if (!der || !len)
		return -EINVAL;

	/* A connection kept open was set up with the previous trust anchor. */
	ipa_http_close(ctx);
	return http_tls_set_ca_cert_der(ctx->tls, der, len);
}

/*! Set the TLS trust anchor from a DER-encoded SubjectPublicKeyInfo blob.
 *  Used for the eUICC's trustedEimPkTls, which carries only the public key of
 *  the eIM's TLS trust anchor (typically a self-signed root CA) rather than a
 *  certificate.  The server chain is verified normally; the otherwise-untrusted
 *  certificate at the top of the chain is accepted iff its public key is this
 *  one.  See the TLS library specific implementations of http_tls.h.
 *  \param[in] spki   DER-encoded SubjectPublicKeyInfo bytes.
 *  \param[in] len    byte length of spki.
 *  \returns 0 on success, -EINVAL on failure. */
int ipa_http_set_ca_pk_spki(void *http_ctx, const uint8_t *spki, size_t len)
{
	struct http_ctx *ctx = http_ctx;

	assert(ctx);
	if (!spki || !len)
		return -EINVAL;

	ipa_http_close(ctx);
	return http_tls_set_ca_pk_spki(ctx->tls, spki, len);
}

/* Callback function to extract the HTTP response */
static size_t store_response_cb(void *ptr, size_t size, size_t nmemb, void *clientp)
{
	struct ipa_buf *buf = *(struct ipa_buf **)clientp;
	size_t realloc_size;

	if (buf->len + size * nmemb > buf->data_len) {
		realloc_size = ((buf->len + size * nmemb) / IPA_LEN_HTTP_RESPONSE_BUF + 1) * IPA_LEN_HTTP_RESPONSE_BUF;
		IPA_LOGP(SIPA, LDEBUG,
			 "HTTP response buffer exhausted, reallocating more memory (have: %zu bytes, required: %zu bytes, will allocate: %zu bytes)\n",
			 buf->data_len, buf->len + size * nmemb, realloc_size);
		buf = ipa_buf_realloc(buf, realloc_size);
		assert(buf);
		*(struct ipa_buf **)clientp = buf;
	}

	memcpy(buf->data + buf->len, ptr, size * nmemb);
	buf->len += size * nmemb;

	return size * nmemb;
}

/*! Open a TCP connection (if not already present) and Perform HTTP request.
 *  \param[inout] http_ctx HTTP client context.
 *  \param[in] req buffer with HTTP request (POST).
 *  \param[in] url URL with HTTP request.
 *  \returns HTTP response on success, NULL on failure. */
struct ipa_buf *ipa_http_req(void *http_ctx, const struct ipa_buf *req, const char *url)
{
	return ipa_http_req_with_ct(http_ctx, req, url, NULL);
}

struct ipa_buf *ipa_http_req_with_ct(void *http_ctx, const struct ipa_buf *req,
				     const char *url, const char *content_type)
{
	struct http_ctx *ctx = http_ctx;
	CURLcode rc;
	struct curl_slist *list = NULL;
	struct ipa_buf *res = ipa_buf_alloc(IPA_LEN_HTTP_RESPONSE_BUF);
	char ct_header[128];

	assert(ctx->initialized);

	/* Create a new curl context (also represents an ongoing connection) in case it does not exist */
	if (!ctx->curl) {
		ctx->curl = curl_easy_init();
		if (!ctx->curl) {
			IPA_LOGP(SHTTP, LERROR, "internal HTTP-client failure!\n");
			goto error;
		}
	}

	/* TLS server verification.  An eUICC-provisioned trust anchor is installed
	 * by http_tls_ssl_ctx_cb (below), in the TLS library's own configuration,
	 * with no ordering dependency on when curl fires the callback vs. when it
	 * loads CAs. */
	if (http_tls_has_ca_cert(ctx->tls)) {
		IPA_LOGP(SHTTP, LDEBUG, "TLS CA: eUICC trustedCertificateTls\n");
	} else if (http_tls_has_ca_pk(ctx->tls)) {
		IPA_LOGP(SHTTP, LDEBUG,
			 "TLS CA: eUICC trustedEimPkTls trust-anchor public key\n");
	} else if (ctx->cabundle) {
		IPA_LOGP(SHTTP, LDEBUG, "TLS CA: file '%s'\n", ctx->cabundle);
		rc = curl_easy_setopt(ctx->curl, CURLOPT_CAINFO, ctx->cabundle);
		if (rc != CURLE_OK) {
			IPA_LOGP(SHTTP, LERROR, "internal HTTP-client failure: %s\n",
				 curl_easy_strerror(rc));
			goto error;
		}
	} else {
		IPA_LOGP(SHTTP, LDEBUG, "TLS CA: system CAs (no eUICC cert, no -C flag)\n");
	}

	/* The ssl_ctx callback installs the eUICC trust anchor. Not while verification is
	 * off: the callback turns verification on in the TLS library by itself. On a
	 * handle kept from an earlier request, a callback installed then is removed. */
	if ((http_tls_has_ca_cert(ctx->tls) || http_tls_has_ca_pk(ctx->tls)) && !ctx->no_verif) {
		rc = curl_easy_setopt(ctx->curl, CURLOPT_SSL_CTX_FUNCTION, http_tls_ssl_ctx_cb);
		if (rc != CURLE_OK) {
			if (rc == CURLE_NOT_BUILT_IN)
				IPA_LOGP(SHTTP, LERROR,
					 "this libcurl does not support CURLOPT_SSL_CTX_FUNCTION, so the "
					 "TLS credentials stored in the eUICC cannot be installed. "
					 "libcurl must be built against %s. "
					 "As a stopgap, pass the eIM certificate with -C <cabundle>.\n",
					 http_tls_backend_name());
			else
				IPA_LOGP(SHTTP, LERROR,
					 "cannot set CURLOPT_SSL_CTX_FUNCTION: %s\n",
					 curl_easy_strerror(rc));
			goto error;
		}
		rc = curl_easy_setopt(ctx->curl, CURLOPT_SSL_CTX_DATA, ctx->tls);
		if (rc != CURLE_OK) {
			IPA_LOGP(SHTTP, LERROR, "cannot set CURLOPT_SSL_CTX_DATA: %s\n",
				 curl_easy_strerror(rc));
			goto error;
		}
	} else {
		curl_easy_setopt(ctx->curl, CURLOPT_SSL_CTX_FUNCTION, NULL);
	}

	if (ctx->no_verif) {
		/* Bypass SSL certificate verification (only for debug, disable in productive use!) */
		rc = curl_easy_setopt(ctx->curl, CURLOPT_SSL_VERIFYPEER, 0L);
		if (rc != CURLE_OK) {
			IPA_LOGP(SHTTP, LERROR, "internal HTTP-client failure: %s\n", curl_easy_strerror(rc));
			goto error;
		}

		/* Bypass SSL hostname verification (only for debug, disable in productive use!) */
		rc = curl_easy_setopt(ctx->curl, CURLOPT_SSL_VERIFYHOST, 0L);
		if (rc != CURLE_OK) {
			IPA_LOGP(SHTTP, LERROR, "internal HTTP-client failure: %s\n", curl_easy_strerror(rc));
			goto error;
		}
		IPA_LOGP(SHTTP, LINFO, "security disabled: will not verify server certificate and hostname\n");
	}

	/* Setup header, see also SGP.32, section 6.1.1 */
	/* UPDATE for v1.2: CR111005R00 — User-Agent now has a spec-defined value;
	 * see IPA_HTTP_USER_AGENT in onomondo/ipa/http_hdr.h.
	 * NEW v1.2 §6.4: content_type may be passed explicitly (JSON binding);
	 * falls back to the ASN.1 default when NULL. */
	list = curl_slist_append(list, "Accept:");
	list = curl_slist_append(list, "User-Agent: " IPA_HTTP_USER_AGENT);
	list = curl_slist_append(list, "X-Admin-Protocol: " IPA_HTTP_X_ADMIN_PROTOCOL);
	if (content_type && *content_type) {
		snprintf(ct_header, sizeof(ct_header), "Content-Type: %s", content_type);
		list = curl_slist_append(list, ct_header);
	} else {
		list = curl_slist_append(list, "Content-Type: " IPA_HTTP_CONTENT_TYPE);
	}
	rc = curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, list);
	if (rc != CURLE_OK) {
		IPA_LOGP(SHTTP, LERROR, "internal HTTP-client failure: %s\n", curl_easy_strerror(rc));
		goto error;
	}

	/* Perform HTTP Request */
	rc = curl_easy_setopt(ctx->curl, CURLOPT_URL, url);
	if (rc != CURLE_OK) {
		IPA_LOGP(SHTTP, LERROR, "internal HTTP-client failure: %s\n", curl_easy_strerror(rc));
		goto error;
	}
	rc = curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, req->data);
	if (rc != CURLE_OK) {
		IPA_LOGP(SHTTP, LERROR, "internal HTTP-client failure: %s\n", curl_easy_strerror(rc));
		goto error;
	}
	rc = curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDSIZE, req->len);
	if (rc != CURLE_OK) {
		IPA_LOGP(SHTTP, LERROR, "internal HTTP-client failure: %s\n", curl_easy_strerror(rc));
		goto error;
	}
	rc = curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, store_response_cb);
	if (rc != CURLE_OK) {
		IPA_LOGP(SHTTP, LERROR, "internal HTTP-client failure: %s\n", curl_easy_strerror(rc));
		goto error;
	}
	rc = curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, (void *)&res);
	if (rc != CURLE_OK) {
		IPA_LOGP(SHTTP, LERROR, "internal HTTP-client failure: %s\n", curl_easy_strerror(rc));
		goto error;
	}
	rc = curl_easy_setopt(ctx->curl, CURLOPT_CONNECTTIMEOUT, ctx->connect_timeout_s);
	if (rc != CURLE_OK) {
		IPA_LOGP(SHTTP, LERROR, "internal HTTP-client failure: %s\n", curl_easy_strerror(rc));
		goto error;
	}
	rc = curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT, ctx->total_timeout_s);
	if (rc != CURLE_OK) {
		IPA_LOGP(SHTTP, LERROR, "internal HTTP-client failure: %s\n", curl_easy_strerror(rc));
		goto error;
	}

	rc = curl_easy_perform(ctx->curl);
	if (rc != CURLE_OK) {
		http_tls_log_failure(ctx->curl);
		IPA_LOGP(SHTTP, LERROR, "HTTP request to %s failed: %s\n", url, curl_easy_strerror(rc));
		goto error;
	}
	IPA_LOGP(SHTTP, LINFO, "HTTP request to %s successful: %s\n", url, curl_easy_strerror(rc));

	curl_slist_free_all(list);
	return res;
error:
	ipa_http_close(http_ctx);
	curl_slist_free_all(list);
	ipa_buf_free(res);
	return NULL;
}

/*! Override the connect and whole-request timeouts (seconds).  A value <= 0
 *  leaves the corresponding timeout at its current setting.  Takes effect on
 *  the next request.
 *  \param[inout] http_ctx HTTP client context.
 *  \param[in] connect_timeout_s TCP connect-phase timeout, or <=0 to keep.
 *  \param[in] total_timeout_s whole-request timeout, or <=0 to keep. */
void ipa_http_set_timeouts(void *http_ctx, long connect_timeout_s, long total_timeout_s)
{
	struct http_ctx *ctx = http_ctx;

	if (!ctx)
		return;
	if (connect_timeout_s > 0)
		ctx->connect_timeout_s = connect_timeout_s;
	if (total_timeout_s > 0)
		ctx->total_timeout_s = total_timeout_s;
}

/*! Close the TCP underlying TCP connection (to be called after the last request).
 *  \param[inout] http_ctx HTTP client context. */
void ipa_http_close(void *http_ctx)
{
	struct http_ctx *ctx = http_ctx;
	if (!ctx->curl)
		return;
	curl_easy_cleanup(ctx->curl);
	ctx->curl = NULL;
}

/*! Free HTTP client.
 *  \param[inout] http_ctx HTTP client context. */
void ipa_http_free(void *http_ctx)
{
	struct http_ctx *ctx = http_ctx;

	if (!http_ctx)
		return;

	ipa_http_close(http_ctx);

	http_tls_free(ctx->tls);
	ctx->tls = NULL;
	/* Only the last live HTTP context tears down the curl global state
	 * (see curl_global_refcnt note in ipa_http_init). */
	if (ctx->initialized && curl_global_refcnt > 0 && --curl_global_refcnt == 0)
		curl_global_cleanup();

	IPA_FREE(ctx);
	IPA_LOGP(SHTTP, LINFO, "HTTP client freed.\n");
}
