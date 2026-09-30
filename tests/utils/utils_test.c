/*
 * Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Author: Philipp Maier <pmaier@sysmocom.de> / sysmocom - s.f.m.c. GmbH
 */

#include <stdio.h>
#include <stdbool.h>
#include <onomondo/ipa/utils.h>
#include "src/ipa/libipa/utils.h"

void ipa_tag_in_taglist_test(void)
{
	uint8_t _tag_list[] = { 0x80, 0xBF, 0x20, 0xBF, 0x22, 0x83, 0x84, 0xA5, 0xA6, 0x88, 0xA9, 0xBF, 0x2B };
	struct ipa_buf *tag_list;
	bool rc;

	tag_list = ipa_buf_alloc_data(sizeof(_tag_list), _tag_list);

	rc = ipa_tag_in_taglist(0x80, tag_list);
	assert(rc == true);
	rc = ipa_tag_in_taglist(0xBF20, tag_list);
	assert(rc == true);
	rc = ipa_tag_in_taglist(0xBF2B, tag_list);
	assert(rc == true);
	rc = ipa_tag_in_taglist(0xA5, tag_list);
	assert(rc == true);
	rc = ipa_tag_in_taglist(0x22, tag_list);
	assert(rc == false);
	rc = ipa_tag_in_taglist(0xBF, tag_list);
	assert(rc == false);
	rc = ipa_tag_in_taglist(0x2B, tag_list);
	assert(rc == false);
	rc = ipa_tag_in_taglist(0xA3, tag_list);
	assert(rc == false);
	rc = ipa_tag_in_taglist(0x81, tag_list);
	assert(rc == false);
	rc = ipa_tag_in_taglist(0xFF, tag_list);
	assert(rc == false);
	rc = ipa_tag_in_taglist(0x00, tag_list);
	assert(rc == false);
	rc = ipa_tag_in_taglist(0xBF23, tag_list);
	assert(rc == false);
	rc = ipa_tag_in_taglist(0xBF00, tag_list);
	assert(rc == false);
	rc = ipa_tag_in_taglist(0xBFFF, tag_list);
	assert(rc == false);

	IPA_FREE(tag_list);
}

void ipa_parse_btlv_hdr_test(void)
{
	size_t len;
	uint16_t tag;
	size_t hdr;

	/* Regression: a short-form length octet of 0x7f (127) must be parsed as a
	 * valid short-form length, not mistaken for a long-form indicator.  The old
	 * `*data < 0x7f` test rejected exactly this value. */
	{
		uint8_t d[] = { 0x80, 0x7f };
		struct ipa_buf *b = ipa_buf_alloc_data(sizeof(d), d);
		hdr = ipa_parse_btlv_hdr(&len, &tag, b);
		assert(hdr == 2);
		assert(tag == 0x80);
		assert(len == 127);
		IPA_FREE(b);
	}

	/* Short-form boundary just below (126). */
	{
		uint8_t d[] = { 0x80, 0x7e };
		struct ipa_buf *b = ipa_buf_alloc_data(sizeof(d), d);
		hdr = ipa_parse_btlv_hdr(&len, &tag, b);
		assert(hdr == 2 && len == 126);
		IPA_FREE(b);
	}

	/* Long-form: 0x81 0x80 encodes length 128 in one extra octet. */
	{
		uint8_t d[] = { 0x80, 0x81, 0x80 };
		struct ipa_buf *b = ipa_buf_alloc_data(sizeof(d), d);
		hdr = ipa_parse_btlv_hdr(&len, &tag, b);
		assert(hdr == 3 && len == 128);
		IPA_FREE(b);
	}

	/* Two-byte tag (0xBF2B) with a short-form length. */
	{
		uint8_t d[] = { 0xBF, 0x2B, 0x05 };
		struct ipa_buf *b = ipa_buf_alloc_data(sizeof(d), d);
		hdr = ipa_parse_btlv_hdr(&len, &tag, b);
		assert(hdr == 3 && tag == 0xBF2B && len == 5);
		IPA_FREE(b);
	}
}

void ipa_strip_tlv_envelope_test(void)
{
	/* Matching envelope: the 2-byte header is chopped, value remains. */
	{
		uint8_t d[] = { 0x88, 0x02, 0xAA, 0xBB };
		int n = ipa_strip_tlv_envelope(d, sizeof(d), 0x88);
		assert(n == 2);
		assert(d[0] == 0xAA && d[1] == 0xBB);
	}

	/* Tag mismatch: buffer is left untouched. */
	{
		uint8_t d[] = { 0x87, 0x02, 0xAA, 0xBB };
		int n = ipa_strip_tlv_envelope(d, sizeof(d), 0x88);
		assert(n == (int)sizeof(d));
	}

	/* Finding #5: an invalid/truncated header (2-byte tag indicated but only
	 * one byte present) makes parse_btlv_hdr return a negative error.  The
	 * caller must detect that (the previously-dead `chop_bytes < 0` check) and
	 * leave the buffer untouched rather than reading an uninitialised tag. */
	{
		uint8_t d[] = { 0xBF };
		int n = ipa_strip_tlv_envelope(d, sizeof(d), 0x88);
		assert(n == 1);
	}
}


/* ipa_retry_after_from_header(): only the delta-seconds form of RFC 9110 10.2.3 is
 * accepted, and the line arrives as libcurl hands it over -- not NUL terminated,
 * CRLF still attached. */
static void ipa_retry_after_from_header_test(void)
{
	struct {
		const char *line;
		bool ok;
		long secs;
	} cases[] = {
		/* the case the eIM actually sends */
		{ "Retry-After: 1200\r\n",		true,	1200 },
		/* header names are case insensitive */
		{ "retry-after: 1200\r\n",		true,	1200 },
		{ "RETRY-AFTER: 1200\r\n",		true,	1200 },
		/* optional whitespace, and none at all */
		{ "Retry-After:1200\r\n",		true,	1200 },
		{ "Retry-After: \t 1200\r\n",		true,	1200 },
		/* bare LF, and no line ending at all */
		{ "Retry-After: 1200\n",		true,	1200 },
		{ "Retry-After: 1200",			true,	1200 },
		/* zero is a legitimate "come back at once" */
		{ "Retry-After: 0\r\n",			true,	0 },
		/* an HTTP-date is rejected rather than guessed at */
		{ "Retry-After: Wed, 21 Oct 2026 07:28:00 GMT\r\n", false, 0 },
		/* rubbish, partial numbers and negatives */
		{ "Retry-After: soon\r\n",		false,	0 },
		{ "Retry-After: 12x\r\n",		false,	0 },
		{ "Retry-After: -60\r\n",		false,	0 },
		{ "Retry-After: \r\n",			false,	0 },
		{ "Retry-After:\r\n",			false,	0 },
		/* a value too long to be a usable delta-seconds */
		{ "Retry-After: 111111111111111111111111111111111111\r\n", false, 0 },
		/* other headers, and ones that merely start the same way */
		{ "Content-Type: application/json\r\n",	false,	0 },
		{ "Retry-Afterwards: 5\r\n",		false,	0 },
		{ "Retry-After\r\n",			false,	0 },
		/* the status line and the terminating empty line libcurl also delivers */
		{ "HTTP/1.1 200 OK\r\n",		false,	0 },
		{ "\r\n",				false,	0 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		long secs = -12345;
		bool ok = ipa_retry_after_from_header(cases[i].line, strlen(cases[i].line), &secs);

		/* Print the line without its line ending, or the report is unreadable. */
		char shown[80];
		size_t n = strcspn(cases[i].line, "\r\n");

		if (n >= sizeof(shown))
			n = sizeof(shown) - 1;
		memcpy(shown, cases[i].line, n);
		shown[n] = '\0';
		printf("  %-46s -> %s", shown, ok ? "ok" : "rejected");
		if (ok)
			printf(", %ld s", secs);
		printf("\n");
		assert(ok == cases[i].ok);
		if (ok)
			assert(secs == cases[i].secs);
		else
			assert(secs == -12345); /* untouched on rejection */
	}

	/* Defensive: a NULL line or output pointer is a rejection, not a crash. */
	{
		long secs = 0;

		assert(ipa_retry_after_from_header(NULL, 10, &secs) == false);
		assert(ipa_retry_after_from_header("Retry-After: 1\r\n", 17, NULL) == false);
	}
}

int main(int argc, char **argv)
{
	ipa_tag_in_taglist_test();
	ipa_parse_btlv_hdr_test();
	ipa_strip_tlv_envelope_test();
	ipa_retry_after_from_header_test();
	return 0;
}

/* Stubs */
void *ipa_http_init(const char *cabundle, bool no_verif)
{
	return NULL;
}

struct ipa_buf *ipa_http_req(void *http_ctx, const struct ipa_buf *req, const char *url)
{
	return NULL;
}

void ipa_http_close(void *http_ctx)
{
	return;
}

void ipa_http_free(void *http_ctx)
{
	return;
}

long ipa_http_get_retry_after(void *http_ctx)
{
	(void)http_ctx;
	return -1;
}

void *ipa_scard_init(unsigned int reader_num)
{
	return NULL;
}

int ipa_scard_reset(void *scard_ctx)
{
	return 0;
}

int ipa_scard_atr(void *scard_ctx, struct ipa_buf *atr)
{
	return 0;
}

int ipa_scard_transceive(void *scard_ctx, struct ipa_buf *res, const struct ipa_buf *req)
{
	return 0;
}

int ipa_scard_free(void *scard_ctx)
{
	return 0;
}

bool ipa_scard_manages_channel(void *scard_ctx)
{
	return false;
}
