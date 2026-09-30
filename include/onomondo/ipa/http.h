/*
 * Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
struct ipa_buf;

/* This is the initial buffer size. The HTTP client will automatically re-alloc more memory if needed. */
#define IPA_LEN_HTTP_RESPONSE_BUF 512	/* bytes */

void *ipa_http_init(const char *cabundle, bool no_verif);
struct ipa_buf *ipa_http_req(void *http_ctx, const struct ipa_buf *req, const char *url);
/* NEW v1.2 §6.4: variant taking an explicit Content-Type header so the
 * JSON binding can request application/json while the ASN.1 binding keeps
 * application/x-gsma-rsp-asn1.  Passing NULL content_type behaves identically
 * to ipa_http_req() (ASN.1 default). */
struct ipa_buf *ipa_http_req_with_ct(void *http_ctx, const struct ipa_buf *req,
				     const char *url, const char *content_type);
void ipa_http_close(void *http_ctx);

/* Value of the `Retry-After` response header of the most recent request, in
 * seconds, or -1 when the last response carried no such header (or one this
 * client could not read).  Reset at the start of every request, so it always
 * describes the response just received.
 *
 * Not part of SGP.32: some eIM deployments use it to steer when the IPAd should
 * come back for the next getPackage.  The HTTP client only reports it; whether
 * it is obeyed is decided further up, see ipa_config.honour_retry_after.
 *
 * Only the delta-seconds form of RFC 9110 section 10.2.3 is understood; an
 * HTTP-date is reported as absent, because the eIM feature this serves is
 * specified in seconds and a date would need a trusted clock the device may
 * not have. */
long ipa_http_get_retry_after(void *http_ctx);
void ipa_http_free(void *http_ctx);

/* Override the connect-phase and whole-request timeouts (seconds); a value
 * <= 0 keeps the current setting.  Defaults are set at ipa_http_init() time
 * (connect 10s, total 300s) and are generous enough for a BoundProfilePackage
 * download.  Takes effect on the next request. */
void ipa_http_set_timeouts(void *http_ctx, long connect_timeout_s, long total_timeout_s);

/* Set the CA certificate used to verify the eIM TLS server, from a
 * DER-encoded X.509 blob (eUICC trustedCertificateTls).
 * Returns 0 on success, -EINVAL on parse failure. */
int ipa_http_set_ca_cert_der(void *http_ctx, const uint8_t *der, size_t len);

/* Set the TLS trust anchor from a DER-encoded SubjectPublicKeyInfo blob
 * (eUICC trustedEimPkTls), which carries only the public key of the eIM's
 * trust anchor rather than a certificate.  The server chain is verified in
 * full; the otherwise-untrusted certificate at the top of the chain is
 * accepted iff its public key is this one.
 * Returns 0 on success, -EINVAL on parse failure. */
int ipa_http_set_ca_pk_spki(void *http_ctx, const uint8_t *spki, size_t len);

