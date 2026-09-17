/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * OpenSSL implementation of http_tls.h. The code was moved here from http.c unchanged in substance.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/mem.h>
#include <onomondo/ipa/utils.h>
#include "http_tls.h"

struct http_tls {
	char *ca_pem;          /* PEM text of CA cert (trustedCertificateTls) */
	size_t ca_pem_len;
	EVP_PKEY *ca_pk;       /* trust-anchor public key (trustedEimPkTls) */
};

const char *http_tls_backend_name(void)
{
	return "OpenSSL";
}

bool http_tls_curl_matches(void)
{
	const curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);

	if (!vi || !vi->ssl_version)
		return false;

	return strstr(vi->ssl_version, "OpenSSL") ||
	       strstr(vi->ssl_version, "BoringSSL") ||
	       strstr(vi->ssl_version, "LibreSSL") ||
	       strstr(vi->ssl_version, "quictls");
}

struct http_tls *http_tls_alloc(void)
{
	return IPA_ALLOC_ZERO(struct http_tls);
}

void http_tls_free(struct http_tls *tls)
{
	if (!tls)
		return;
	IPA_FREE(tls->ca_pem);
	if (tls->ca_pk)
		EVP_PKEY_free(tls->ca_pk);
	IPA_FREE(tls);
}

bool http_tls_has_ca_cert(const struct http_tls *tls)
{
	return tls->ca_pem != NULL;
}

bool http_tls_has_ca_pk(const struct http_tls *tls)
{
	return tls->ca_pk != NULL;
}

/* -------------------------------------------------------------------------
 * Trust-anchor-by-public-key verification (SGP.32 trustedEimPkTls).
 *
 * The eUICC may store only the *public key* of the eIM's TLS trust anchor,
 * not a certificate.  OpenSSL's trust store is cert-based, so we cannot add
 * a bare key to it.  Instead we let the normal chain verification run and
 * hook the one error it necessarily raises — the top-of-chain certificate is
 * not a known CA — and accept it iff its public key is exactly the key the
 * eUICC provisioned.
 *
 * All other checks (per-link signatures, validity dates, hostname) still run
 * and can still fail: OpenSSL calls the callback again for each of them.  The
 * chain is therefore verified up to a root whose key came from the eUICC,
 * which is precisely the trust decision SGP.32 asks for.
 * ------------------------------------------------------------------------- */
static int g_httpctx_ssl_ex_idx = -1;

static int trust_anchor_verify_cb(int preverify_ok, X509_STORE_CTX *store_ctx)
{
	SSL *ssl;
	SSL_CTX *ssl_ctx;
	struct http_tls *tls;
	X509 *cur;
	EVP_PKEY *cur_pk;
	int err;

	if (preverify_ok)
		return 1;

	err = X509_STORE_CTX_get_error(store_ctx);

	/* Only the "chain does not end in a locally trusted CA" family of errors
	 * is eligible for a public-key trust override.  Anything else (expired,
	 * bad signature, hostname mismatch, ...) is a genuine failure. */
	switch (err) {
	case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
	case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
	case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
	case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
	case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
	case X509_V_ERR_CERT_UNTRUSTED:
		break;
	default:
		return 0;
	}

	ssl = X509_STORE_CTX_get_ex_data(store_ctx,
					 SSL_get_ex_data_X509_STORE_CTX_idx());
	if (!ssl)
		return 0;
	ssl_ctx = SSL_get_SSL_CTX(ssl);
	tls = ssl_ctx ? SSL_CTX_get_ex_data(ssl_ctx, g_httpctx_ssl_ex_idx) : NULL;
	if (!tls || !tls->ca_pk)
		return 0;

	cur = X509_STORE_CTX_get_current_cert(store_ctx);
	cur_pk = cur ? X509_get0_pubkey(cur) : NULL;
	if (!cur_pk)
		return 0;

	if (EVP_PKEY_eq(tls->ca_pk, cur_pk) != 1) {
		IPA_LOGP(SHTTP, LERROR,
			 "TLS chain ends in an untrusted certificate whose public key "
			 "does not match the trust anchor stored in the eUICC\n");
		return 0;
	}

	IPA_LOGP(SHTTP, LDEBUG,
		 "TLS trust anchor accepted: public key matches eUICC trustedEimPkTls\n");
	X509_STORE_CTX_set_error(store_ctx, X509_V_OK);
	return 1;
}

/* -------------------------------------------------------------------------
 * CURLOPT_SSL_CTX_FUNCTION callback.  Installs whichever eUICC-provisioned
 * TLS credentials are present:
 *
 *  1. trustedCertificateTls: add the cert to the trust store and set
 *     X509_V_FLAG_PARTIAL_CHAIN, so a leaf or intermediate stored in the
 *     eUICC can act as a direct trust anchor (SGP.32 §3.1 encourages
 *     self-signed certs, which need not chain to a public root).
 *  2. trustedEimPkTls: install trust_anchor_verify_cb (see above).
 * ------------------------------------------------------------------------- */
CURLcode http_tls_ssl_ctx_cb(CURL *curl, void *ssl_ctx_void, void *clientp)
{
	struct http_tls *tls = clientp;
	SSL_CTX *ssl_ctx = ssl_ctx_void;
	(void)curl;

	if (tls->ca_pem) {
		X509_STORE *st = SSL_CTX_get_cert_store(ssl_ctx);
		BIO *bio = BIO_new_mem_buf(tls->ca_pem, (int)tls->ca_pem_len);
		if (bio) {
			X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
			BIO_free(bio);
			if (cert) {
				X509_STORE_add_cert(st, cert);
				X509_free(cert);
			}
		}
		X509_STORE_set_flags(st, X509_V_FLAG_PARTIAL_CHAIN);
	}

	/* trustedEimPkTls: the eUICC gave us a bare trust-anchor public key.
	 * Accept the otherwise-untrusted top of the chain iff its key matches. */
	if (tls->ca_pk) {
		SSL_CTX_set_ex_data(ssl_ctx, g_httpctx_ssl_ex_idx, tls);
		SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, trust_anchor_verify_cb);
	}

	return CURLE_OK;
}

int http_tls_set_ca_cert_der(struct http_tls *tls, const uint8_t *der, size_t len)
{
	const unsigned char *p = der;
	X509 *cert;
	char subj[256];
	BIO *bio;
	char *pem_data;
	long pem_len;

	cert = d2i_X509(NULL, &p, (long)len);
	if (!cert) {
		IPA_LOGP(SHTTP, LERROR, "cannot parse DER CA certificate: %s\n",
			 ERR_reason_error_string(ERR_get_error()));
		return -EINVAL;
	}

	X509_NAME_oneline(X509_get_subject_name(cert), subj, sizeof(subj));

	/* The trust store is cert-based and loads PEM, so convert once here. */
	bio = BIO_new(BIO_s_mem());
	if (!bio || !PEM_write_bio_X509(bio, cert)) {
		BIO_free(bio);
		X509_free(cert);
		return -EINVAL;
	}
	pem_len = BIO_get_mem_data(bio, &pem_data);

	IPA_FREE(tls->ca_pem);
	tls->ca_pem = IPA_ALLOC_N(pem_len + 1);
	if (!tls->ca_pem) {
		BIO_free(bio);
		X509_free(cert);
		return -EINVAL;
	}
	memcpy(tls->ca_pem, pem_data, (size_t)pem_len);
	tls->ca_pem[pem_len] = '\0';
	tls->ca_pem_len = (size_t)pem_len;

	IPA_LOGP(SHTTP, LINFO, "eUICC TLS CA certificate installed (subject=%s)\n", subj);

	BIO_free(bio);
	X509_free(cert);
	return 0;
}

int http_tls_set_ca_pk_spki(struct http_tls *tls, const uint8_t *spki, size_t len)
{
	const unsigned char *p = spki;
	EVP_PKEY *pk;

	pk = d2i_PUBKEY(NULL, &p, (long)len);
	if (!pk) {
		IPA_LOGP(SHTTP, LERROR,
			 "cannot parse eUICC trustedEimPkTls SubjectPublicKeyInfo: %s\n",
			 ERR_reason_error_string(ERR_get_error()));
		return -EINVAL;
	}

	if (g_httpctx_ssl_ex_idx < 0)
		g_httpctx_ssl_ex_idx =
		    SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);

	if (tls->ca_pk)
		EVP_PKEY_free(tls->ca_pk);
	tls->ca_pk = pk;

	IPA_LOGP(SHTTP, LINFO,
		 "eUICC TLS trust anchor public key installed (type=%s, bits=%d)\n",
		 OBJ_nid2sn(EVP_PKEY_base_id(pk)), EVP_PKEY_bits(pk));
	return 0;
}

void http_tls_log_failure(CURL *curl)
{
	unsigned long ossl_err;
	long ssl_vr = 0;

	curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &ssl_vr);
	if (ssl_vr != 0)
		IPA_LOGP(SHTTP, LERROR, "SSL verify result: %ld (%s)\n",
			 ssl_vr, X509_verify_cert_error_string(ssl_vr));
	while ((ossl_err = ERR_get_error()) != 0) {
		char errbuf[256];
		ERR_error_string_n(ossl_err, errbuf, sizeof(errbuf));
		IPA_LOGP(SHTTP, LERROR, "OpenSSL error: %s\n", errbuf);
	}
}
