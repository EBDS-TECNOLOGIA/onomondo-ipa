/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Mbed TLS 3.x implementation of http_tls.h, for a libcurl built against Mbed TLS -- the one OpenWrt ships.
 *
 * libcurl hands CURLOPT_SSL_CTX_FUNCTION the mbedtls_ssl_config of the connection, after it has set up the
 * trust store, the verification mode (always MBEDTLS_SSL_VERIFY_REQUIRED) and its own verify callback, and
 * before the handshake. The trust anchors are installed there:
 *
 *  - trustedCertificateTls: the certificate replaces libcurl's trust store (mbedtls_ssl_conf_ca_chain). Any
 *    certificate in the trust store acts as a trust anchor in Mbed TLS, so a CA stored in the eUICC need not be
 *    self-signed. A leaf stored in the eUICC (a pinned server certificate) is handled by the verify callback.
 *    Unlike the OpenSSL implementation, which adds the certificate to the system CAs, the certificate is then
 *    the only trust anchor.
 *
 *  - trustedEimPkTls: the verify callback accepts a chain whose top certificate is untrusted only because no
 *    trusted issuer was found, if that certificate carries exactly the provisioned public key -- the same trust
 *    decision as the OpenSSL implementation. Every other verification error still fails the handshake.
 *
 * The verify callback replaces libcurl's, which only logs, and clears flags when certificate or host name
 * verification is turned off. http.c therefore installs the ssl_ctx callback only while verification is on.
 * The handshake fails on the flags the callback leaves; the callback logs them, since libcurl's Mbed TLS backend
 * does not report them through CURLINFO_SSL_VERIFYRESULT.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <mbedtls/version.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/mem.h>
#include <onomondo/ipa/utils.h>
#include "http_tls.h"

#if MBEDTLS_VERSION_MAJOR < 3
#error "http_tls_mbedtls.c needs Mbed TLS 3.x"
#endif

/* Large enough for the SubjectPublicKeyInfo of an RSA-8192 key. */
#define SPKI_BUF_LEN 2048

struct http_tls {
	/* trustedCertificateTls */
	bool have_cert;
	mbedtls_x509_crt ca_cert;

	/* trustedEimPkTls, as SubjectPublicKeyInfo re-encoded by Mbed TLS, so that it compares byte for byte with
	 * the key of a certificate encoded the same way. */
	unsigned char *pk_der;
	size_t pk_der_len;

	/* State of one chain verification, see verify_cb(). */
	bool untrusted_pending;
	bool anchored;
};

const char *http_tls_backend_name(void)
{
	return "Mbed TLS";
}

bool http_tls_curl_matches(void)
{
	const curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);

	return vi && vi->ssl_version && strstr(vi->ssl_version, "mbedTLS");
}

struct http_tls *http_tls_alloc(void)
{
	struct http_tls *tls = IPA_ALLOC_ZERO(struct http_tls);

	mbedtls_x509_crt_init(&tls->ca_cert);
	return tls;
}

void http_tls_free(struct http_tls *tls)
{
	if (!tls)
		return;
	mbedtls_x509_crt_free(&tls->ca_cert);
	IPA_FREE(tls->pk_der);
	IPA_FREE(tls);
}

bool http_tls_has_ca_cert(const struct http_tls *tls)
{
	return tls->have_cert;
}

bool http_tls_has_ca_pk(const struct http_tls *tls)
{
	return tls->pk_der != NULL;
}

static void log_mbedtls_error(const char *what, int rc)
{
	IPA_LOGP(SHTTP, LERROR, "%s: Mbed TLS error -0x%04x\n", what, (unsigned int)-rc);
}

/* Encode a public key as SubjectPublicKeyInfo into buf. mbedtls_pk_write_pubkey_der() writes at the end of the
 * buffer; *der points at the start of the encoding. Returns its length, or a negative Mbed TLS error. */
static int spki_of(mbedtls_pk_context *pk, unsigned char *buf, size_t buf_len, const unsigned char **der)
{
	int len = mbedtls_pk_write_pubkey_der(pk, buf, buf_len);

	if (len > 0)
		*der = buf + buf_len - (size_t)len;
	return len;
}

int http_tls_set_ca_cert_der(struct http_tls *tls, const uint8_t *der, size_t len)
{
	mbedtls_x509_crt cert;
	char subj[256];
	int rc;

	mbedtls_x509_crt_init(&cert);
	rc = mbedtls_x509_crt_parse_der(&cert, der, len);
	if (rc != 0) {
		log_mbedtls_error("cannot parse DER CA certificate", rc);
		mbedtls_x509_crt_free(&cert);
		return -EINVAL;
	}

	if (mbedtls_x509_dn_gets(subj, sizeof(subj), &cert.subject) < 0)
		strcpy(subj, "?");

	mbedtls_x509_crt_free(&cert);

	/* Parsed once above to validate, so that a bad certificate leaves the previous one in place. */
	mbedtls_x509_crt_free(&tls->ca_cert);
	mbedtls_x509_crt_init(&tls->ca_cert);
	rc = mbedtls_x509_crt_parse_der(&tls->ca_cert, der, len);
	tls->have_cert = rc == 0;
	if (rc != 0) {
		log_mbedtls_error("cannot parse DER CA certificate", rc);
		return -EINVAL;
	}

	IPA_LOGP(SHTTP, LINFO, "eUICC TLS CA certificate installed (subject=%s)\n", subj);
	return 0;
}

int http_tls_set_ca_pk_spki(struct http_tls *tls, const uint8_t *spki, size_t len)
{
	mbedtls_pk_context pk;
	unsigned char *buf;
	const unsigned char *der = NULL;
	int rc;

	mbedtls_pk_init(&pk);
	rc = mbedtls_pk_parse_public_key(&pk, spki, len);
	if (rc != 0) {
		log_mbedtls_error("cannot parse eUICC trustedEimPkTls SubjectPublicKeyInfo", rc);
		mbedtls_pk_free(&pk);
		return -EINVAL;
	}

	buf = IPA_ALLOC_N(SPKI_BUF_LEN);
	rc = spki_of(&pk, buf, SPKI_BUF_LEN, &der);
	if (rc <= 0) {
		log_mbedtls_error("cannot encode eUICC trustedEimPkTls", rc);
		IPA_FREE(buf);
		mbedtls_pk_free(&pk);
		return -EINVAL;
	}

	IPA_FREE(tls->pk_der);
	tls->pk_der = IPA_ALLOC_N((size_t)rc);
	memcpy(tls->pk_der, der, (size_t)rc);
	tls->pk_der_len = (size_t)rc;

	IPA_LOGP(SHTTP, LINFO, "eUICC TLS trust anchor public key installed (type=%s, bits=%zu)\n",
		 mbedtls_pk_get_name(&pk), mbedtls_pk_get_bitlen(&pk));

	IPA_FREE(buf);
	mbedtls_pk_free(&pk);
	return 0;
}

static bool key_matches(const struct http_tls *tls, mbedtls_x509_crt *crt)
{
	unsigned char buf[SPKI_BUF_LEN];
	const unsigned char *der = NULL;
	int len;

	if (!tls->pk_der)
		return false;
	len = spki_of(&crt->pk, buf, sizeof(buf), &der);
	return len > 0 && (size_t)len == tls->pk_der_len && memcmp(der, tls->pk_der, tls->pk_der_len) == 0;
}

static bool is_pinned_cert(const struct http_tls *tls, const mbedtls_x509_crt *crt)
{
	return tls->have_cert && crt->raw.len == tls->ca_cert.raw.len &&
	       memcmp(crt->raw.p, tls->ca_cert.raw.p, crt->raw.len) == 0;
}

/* A human readable form of verification flags. */
static void flags_info(char *buf, size_t buf_len, uint32_t flags)
{
#ifndef MBEDTLS_X509_REMOVE_INFO
	if (mbedtls_x509_crt_verify_info(buf, buf_len, "", flags) >= 0) {
		/* One line per flag; the log line is the last one. */
		size_t len = strlen(buf);
		char *nl;

		while (len && buf[len - 1] == '\n')
			buf[--len] = '\0';
		for (nl = strchr(buf, '\n'); nl; nl = strchr(nl, '\n'))
			*nl = ';';
		return;
	}
#endif
	snprintf(buf, buf_len, "flags 0x%08x", (unsigned int)flags);
}

/* Mbed TLS calls this once per certificate of the verified chain, from the top (highest depth) down to the
 * server's certificate (depth 0), with the flags found for that certificate; the flags left behind make up the
 * result. MBEDTLS_X509_BADCERT_NOT_TRUSTED sits on the top certificate when no trusted issuer was found for it.
 *
 * - The top certificate carries the trustedEimPkTls key: the chain is anchored.
 * - trustedCertificateTls is the server's own certificate: the chain is anchored if, and only if, the server
 *   certificate is that very certificate. That is only known at depth 0, after the top certificate's flags have
 *   been merged, so the flag is cleared at the top and put back at depth 0 when the server certificate turns out
 *   to be a different one. */
static int verify_cb(void *p, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
	struct http_tls *tls = p;

	if (*flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
		if (key_matches(tls, crt)) {
			IPA_LOGP(SHTTP, LDEBUG,
				 "TLS trust anchor accepted: public key matches eUICC trustedEimPkTls\n");
			*flags &= ~MBEDTLS_X509_BADCERT_NOT_TRUSTED;
			tls->anchored = true;
		} else if (tls->have_cert) {
			*flags &= ~MBEDTLS_X509_BADCERT_NOT_TRUSTED;
			tls->untrusted_pending = true;
		} else if (tls->pk_der) {
			IPA_LOGP(SHTTP, LERROR,
				 "TLS chain ends in an untrusted certificate whose public key "
				 "does not match the trust anchor stored in the eUICC\n");
		}
	}

	if (depth == 0) {
		if (tls->untrusted_pending && !tls->anchored) {
			if (is_pinned_cert(tls, crt)) {
				IPA_LOGP(SHTTP, LDEBUG,
					 "TLS server certificate is the eUICC trustedCertificateTls\n");
			} else {
				IPA_LOGP(SHTTP, LERROR,
					 "TLS chain does not lead to the certificate stored in the eUICC\n");
				*flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
			}
		}
		/* The chain is done; the next verification starts afresh. */
		tls->untrusted_pending = false;
		tls->anchored = false;
	}

	if (*flags) {
		char info[256];

		flags_info(info, sizeof(info), *flags);
		IPA_LOGP(SHTTP, LERROR, "TLS certificate at depth %d rejected: %s\n", depth, info);
	}
	return 0;
}

CURLcode http_tls_ssl_ctx_cb(CURL *curl, void *ssl_ctx, void *clientp)
{
	struct http_tls *tls = clientp;
	mbedtls_ssl_config *conf = ssl_ctx;
	(void)curl;

	tls->untrusted_pending = false;
	tls->anchored = false;

	if (tls->have_cert)
		mbedtls_ssl_conf_ca_chain(conf, &tls->ca_cert, NULL);
	mbedtls_ssl_conf_verify(conf, verify_cb, tls);
	mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);

	return CURLE_OK;
}

void http_tls_log_failure(CURL *curl)
{
	/* verify_cb() has logged the verification errors already; libcurl's Mbed TLS backend keeps no verify
	 * result for CURLINFO_SSL_VERIFYRESULT, and its error text comes with the curl error http.c logs. */
	(void)curl;
}
