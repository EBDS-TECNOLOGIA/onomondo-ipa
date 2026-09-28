/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/* The ES10x response of an eUICC can reach the IPAd in two shapes, depending on
 * what the transport underneath does with ISO/IEC 7816-4 61xx chaining:
 *
 *   MODEM_CHAINED    STORE DATA answers 61xx and the body is fetched with
 *                    GET RESPONSE.  This is PC/SC, and the modem of the POS the
 *                    Android port was brought up on.
 *   MODEM_ASSEMBLED  the modem/RIL runs the GET RESPONSE chain itself and hands
 *                    the whole body back on the STORE DATA, with SW=9000.
 *
 * Both must produce the same ES10x response.  These tests drive
 * ipa_euicc_transceive_es10x() through a scripted scard mock in each shape.
 */

#include <onomondo/ipa/ipad.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/log.h>
#include "src/ipa/libipa/context.h"
#include "src/ipa/libipa/euicc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define STORE_DATA_INS 0xE2
#define GET_RESPONSE_INS 0xC0

enum modem_kind {
	MODEM_CHAINED,
	MODEM_ASSEMBLED,
};

/* The body the fake eUICC returns: a real GetEID response as captured on
 * hardware (BF3E 12 5A 10 <16 byte EID>), optionally padded out to body_len to
 * exercise responses larger than a single 256 byte block. */
static const uint8_t eid_body[] = {
	0xBF, 0x3E, 0x12, 0x5A, 0x10, 0x89, 0x04, 0x40, 0x45, 0x93, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x17, 0x46, 0x45, 0x36, 0x34
};

static struct {
	enum modem_kind kind;
	uint8_t body[2048];
	size_t body_len;
	size_t sent;		/* bytes already handed over via GET RESPONSE */
	unsigned int get_response_calls;
} modem;

static void modem_reset(enum modem_kind kind, size_t body_len)
{
	size_t i;

	memset(&modem, 0, sizeof(modem));
	modem.kind = kind;
	assert(body_len <= sizeof(modem.body));
	modem.body_len = body_len;
	for (i = 0; i < body_len; i++)
		modem.body[i] = i < sizeof(eid_body) ? eid_body[i] : (uint8_t) i;
}

static void reply(struct ipa_buf *res, const uint8_t *data, size_t len, uint16_t sw)
{
	assert(len + 2 <= res->data_len);
	if (len)
		memcpy(res->data, data, len);
	res->data[len] = sw >> 8;
	res->data[len + 1] = sw & 0xff;
	res->len = len + 2;
}

/* --- scard mock ---------------------------------------------------------- */

void *ipa_scard_init(unsigned int reader_num)
{
	return &modem;
}

int ipa_scard_reset(void *scard_ctx)
{
	return 0;
}

int ipa_scard_atr(void *scard_ctx, struct ipa_buf *atr)
{
	return 0;
}

int ipa_scard_free(void *scard_ctx)
{
	return 0;
}

bool ipa_scard_manages_channel(void *scard_ctx)
{
	/* The interesting case is the Android transport, which owns the channel. */
	return true;
}

int ipa_scard_transceive(void *scard_ctx, struct ipa_buf *res, const struct ipa_buf *req)
{
	size_t remaining, chunk;

	assert(req->len >= 4);

	if (req->data[1] == STORE_DATA_INS) {
		if (modem.kind == MODEM_ASSEMBLED) {
			/* Whole body up front, no chaining advertised. */
			reply(res, modem.body, modem.body_len, 0x9000);
			modem.sent = modem.body_len;
			return 0;
		}
		/* Chaining: announce how much is waiting (0 means 256). */
		chunk = modem.body_len > 256 ? 256 : modem.body_len;
		reply(res, NULL, 0, 0x6100 | (chunk & 0xff));
		return 0;
	}

	if (req->data[1] == GET_RESPONSE_INS) {
		modem.get_response_calls++;
		remaining = modem.body_len - modem.sent;
		chunk = req->data[4] ? req->data[4] : 256;
		if (chunk > remaining)
			chunk = remaining;
		modem.sent += chunk;
		remaining = modem.body_len - modem.sent;
		if (remaining == 0) {
			reply(res, modem.body + modem.sent - chunk, chunk, 0x9000);
		} else {
			reply(res, modem.body + modem.sent - chunk, chunk,
			      0x6100 | (remaining > 256 ? 0x00 : (remaining & 0xff)));
		}
		return 0;
	}

	assert(!"unexpected APDU");
	return -1;
}

/* --- tests --------------------------------------------------------------- */

static void run_case(const char *name, enum modem_kind kind, size_t body_len)
{
	struct ipa_config cfg = { 0 };
	struct ipa_context ctx = { 0 };
	struct ipa_buf *req;
	struct ipa_buf *res;

	cfg.euicc_channel = 1;
	ctx.cfg = &cfg;

	modem_reset(kind, body_len);

	/* ES10c GetEID request, as euicc.c would hand it to the transport. */
	req = ipa_buf_alloc(6);
	assert(req);
	memcpy(req->data, "\xBF\x3E\x03\x5C\x01\x5A", 6);
	req->len = 6;

	res = ipa_euicc_transceive_es10x(&ctx, req);

	printf("%-34s body=%4zu -> ", name, body_len);
	if (!res) {
		printf("FAIL (no response)\n");
		assert(!"transceive returned NULL");
	}
	printf("got %4zu bytes, %u GET RESPONSE call(s)", res->len, modem.get_response_calls);
	assert(res->len == body_len);
	assert(memcmp(res->data, modem.body, body_len) == 0);
	printf(" -- OK\n");

	ipa_buf_free(res);
	ipa_buf_free(req);
}

/* Keep stderr out of the way; the test is checked against its stdout. */
static void discard_log(const char *line, size_t len)
{
}

int main(void)
{
	/* Unbuffered, so a failing assert still shows which case got that far. */
	setvbuf(stdout, NULL, _IONBF, 0);

	ipa_log_set_sink(discard_log);

	/* The POS that worked: modem exposes 61xx, core runs GET RESPONSE. */
	run_case("chained modem, short body", MODEM_CHAINED, sizeof(eid_body));
	/* The POS that failed: modem assembled the chain, SW=9000 with the body. */
	run_case("assembled modem, short body", MODEM_ASSEMBLED, sizeof(eid_body));
	/* Larger than one block: exercises the 256 byte block and the realloc. */
	run_case("chained modem, multi block", MODEM_CHAINED, 600);
	/* Larger than the old 257 byte STORE DATA receive buffer. */
	run_case("assembled modem, large body", MODEM_ASSEMBLED, 600);

	printf("all es10x chaining cases passed\n");
	return 0;
}
