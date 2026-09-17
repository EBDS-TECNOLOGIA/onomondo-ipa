/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

/* ===========================================================================
 * TLS library specific part of http.c
 * ===========================================================================
 *
 * The eUICC provisions the eIM's TLS trust anchor (SGP.32 trustedPublicKeyDataTls), either as a certificate
 * (trustedCertificateTls) or as a bare public key (trustedEimPkTls). libcurl has no portable option for either,
 * so http.c installs them through CURLOPT_SSL_CTX_FUNCTION, whose argument is the TLS library's own
 * configuration object. Everything that touches that object lives behind this interface, with one
 * implementation per TLS library, chosen when the project is configured (-DIPA_HTTP_TLS):
 *
 *   http_tls_openssl.c  OpenSSL and its forks (the callback receives an SSL_CTX *)
 *   http_tls_mbedtls.c  Mbed TLS 3.x (the callback receives an mbedtls_ssl_config *), as OpenWrt's libcurl uses
 *
 * libcurl must be built against the same library, which http_tls_curl_matches() checks at run time.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <curl/curl.h>

/* Trust anchors of one HTTP client context. */
struct http_tls;

/*! Name of the TLS library this build was made for, for log messages. */
const char *http_tls_backend_name(void);

/*! Whether the libcurl in use runs on that TLS library. When it does not, the ssl_ctx callback would be handed
 *  an object of another library, so the trust anchors cannot be used at all. */
bool http_tls_curl_matches(void);

struct http_tls *http_tls_alloc(void);
void http_tls_free(struct http_tls *tls);

/*! Install a trust anchor certificate (DER). Replaces a previous one. 0 or -EINVAL. */
int http_tls_set_ca_cert_der(struct http_tls *tls, const uint8_t *der, size_t len);

/*! Install a trust anchor public key (DER SubjectPublicKeyInfo). Replaces a previous one. 0 or -EINVAL. */
int http_tls_set_ca_pk_spki(struct http_tls *tls, const uint8_t *spki, size_t len);

bool http_tls_has_ca_cert(const struct http_tls *tls);
bool http_tls_has_ca_pk(const struct http_tls *tls);

/*! CURLOPT_SSL_CTX_FUNCTION callback; CURLOPT_SSL_CTX_DATA must be the struct http_tls. Only to be installed
 *  when certificate verification is on: it enables verification in the TLS library by itself. */
CURLcode http_tls_ssl_ctx_cb(CURL *curl, void *ssl_ctx, void *tls);

/*! Log what the TLS library knows about a failed request, after curl_easy_perform() failed. */
void http_tls_log_failure(CURL *curl);
