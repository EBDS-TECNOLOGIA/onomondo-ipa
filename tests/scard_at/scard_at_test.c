/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The AT+CSIM transport (src/ipa/scard_at.c) and the logical-channel handling it needs from libipa/euicc.c,
 * against a fake modem on a pseudo terminal.
 *
 * The dialogue is the one recorded from a Quectel EC200A with a Thales eUICC, with one change: the card hands
 * out logical channel 5 rather than 1, which is what a modem that keeps channels of its own does, and which
 * makes the run exercise the CLA encoding for channels 4 to 19 (0x41 instead of 0x01, 0xC1 instead of 0x81).
 */

/* posix_openpt(), grantpt(), ptsname_r() */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/ipad.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/scard.h>
#include <onomondo/ipa/scard_transport.h>

/* ------------------------------------------------------------------------
 * The fake modem
 * --------------------------------------------------------------------- */

/* One APDU the card answers, as hex. */
struct card_exchange {
	const char *command;
	const char *response;
};

/* The EID the card reports, as it appears in the GetEID response below. */
#define TEST_EID "89033023931110000000078094766576"

static const struct card_exchange card[] = {
	/* TERMINAL CAPABILITY on the basic channel, as libipa sends it (tag 84: IPAd supported). */
	{ "80AA000005A903840101", "9000" },
	/* MANAGE CHANNEL open, P2=0: the card picks channel 5. */
	{ "0070000001", "059000" },
	/* SELECT ISD-R on channel 5 -> CLA 0x41. */
	{ "41A4040410A0000005591010FFFFFFFF8900000100", "6173" },
	/* GET RESPONSE for the FCI: the real ISD-R FCI of the Thales card, whose E1 template says the eUICC has
	 * an IPAe and an enabled profile. */
	{ "41C0000073",
	  "6F718410A0000005591010FFFFFFFF8900000100A557734806072A864886FC6B01600B06092A864886FC6B020203630906072A"
	  "864886FC6B03650D060B2A864886FC6B0504020000661606"
	  "0A2B060104012A026E0103060847544F30344D010B9F6E06007760B0010B9F6501FFE104800206C09000" },
	/* ES10c GetEID as STORE DATA on channel 5 -> CLA 0xC1. */
	{ "C1E2910006BF3E035C015A", "6115" },
	{ "41C0000015", "BF3E125A10" "89033023931110000000078094766576" "9000" },
	/* MANAGE CHANNEL close of channel 5. */
	{ "00708005", "9000" },
};

static struct {
	int fd;             /* master side of the pty */
	pthread_t thread;
	bool stop;
	bool echo;          /* the modem echoes commands until ATE0 */
	unsigned int csim_count;
	unsigned int urc_before;   /* send an unsolicited line before this many-th +CSIM answer */
	bool cme_error_once;       /* answer the next AT+CSIM with an error */
	bool saw_cfun_off;
	bool saw_cfun_on;
	bool answer_cpin;          /* report the SIM ready after AT+CFUN=1 */
} modem;

static void modem_write(const char *text)
{
	ssize_t rc = write(modem.fd, text, strlen(text));

	(void)rc;
}

static void modem_reply(const char *body)
{
	char buf[4096];

	if (body) {
		snprintf(buf, sizeof(buf), "\r\n%s\r\n", body);
		modem_write(buf);
	}
	modem_write("\r\nOK\r\n");
}

static const char *card_answer(const char *apdu_hex)
{
	unsigned int i;

	for (i = 0; i < IPA_ARRAY_SIZE(card); i++) {
		if (strcasecmp(card[i].command, apdu_hex) == 0)
			return card[i].response;
	}
	return NULL;
}

/* AT+CSIM=<len>,"<hex>" */
static void handle_csim(const char *line)
{
	char apdu[1024];
	char reply[4096];
	const char *quote = strchr(line, '"');
	const char *end = quote ? strchr(quote + 1, '"') : NULL;
	const char *answer;
	size_t len;

	if (!quote || !end) {
		modem_write("\r\n+CME ERROR: 50\r\n");
		return;
	}

	modem.csim_count++;
	if (modem.cme_error_once) {
		modem.cme_error_once = false;
		modem_write("\r\n+CME ERROR: 13\r\n");
		return;
	}
	if (modem.urc_before && modem.csim_count == modem.urc_before)
		modem_write("\r\n+CREG: 1,\"1A2B\",\"01234567\",7\r\n");

	len = (size_t)(end - quote - 1);
	if (len >= sizeof(apdu))
		len = sizeof(apdu) - 1;
	memcpy(apdu, quote + 1, len);
	apdu[len] = '\0';

	answer = card_answer(apdu);
	if (!answer) {
		/* Any APDU the script does not know: "instruction not supported". */
		answer = "6D00";
	}
	snprintf(reply, sizeof(reply), "+CSIM: %zu,\"%s\"", strlen(answer), answer);
	modem_reply(reply);
}

static void handle_line(const char *line)
{
	if (modem.echo) {
		modem_write(line);
		modem_write("\r");
	}

	if (strcmp(line, "ATE0") == 0) {
		modem.echo = false;
		modem_reply(NULL);
	} else if (strcmp(line, "AT+CSIM=?") == 0) {
		modem_reply(NULL);
	} else if (strncmp(line, "AT+CSIM=", 8) == 0) {
		handle_csim(line);
	} else if (strcmp(line, "AT+CFUN=0") == 0) {
		modem.saw_cfun_off = true;
		modem_reply(NULL);
	} else if (strcmp(line, "AT+CFUN=1") == 0) {
		modem.saw_cfun_on = true;
		modem_reply(NULL);
		if (modem.answer_cpin)
			modem_write("\r\n+CPIN: READY\r\n");
	} else if (strncmp(line, "AT", 2) == 0) {
		modem_reply(NULL);
	}
}

static void *modem_thread(void *arg)
{
	char buf[4096];
	size_t len = 0;
	(void)arg;

	while (!modem.stop) {
		struct pollfd pfd = { .fd = modem.fd, .events = POLLIN };
		char *cr;
		ssize_t got;

		if (poll(&pfd, 1, 100) <= 0)
			continue;
		got = read(modem.fd, buf + len, sizeof(buf) - len - 1);
		if (got <= 0)
			continue;
		len += (size_t)got;
		buf[len] = '\0';

		while ((cr = strpbrk(buf, "\r\n")) != NULL) {
			*cr = '\0';
			if (*buf)
				handle_line(buf);
			len -= (size_t)(cr - buf) + 1;
			memmove(buf, cr + 1, len + 1);
		}
	}
	return NULL;
}

static char pts_name[128];

static void modem_start(void)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);

	assert(master >= 0);
	assert(grantpt(master) == 0);
	assert(unlockpt(master) == 0);
	assert(ptsname_r(master, pts_name, sizeof(pts_name)) == 0);

	memset(&modem, 0, sizeof(modem));
	modem.fd = master;
	modem.echo = true; /* until ATE0, as a modem fresh out of reset */
	assert(pthread_create(&modem.thread, NULL, modem_thread, NULL) == 0);
}

static void modem_stop(void)
{
	modem.stop = true;
	pthread_join(modem.thread, NULL);
	close(modem.fd);
}

/* ------------------------------------------------------------------------
 * The eIM side is not exercised here; libipa needs the symbols to link.
 * --------------------------------------------------------------------- */

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

/* ------------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

static char uri[256];

static void uri_for(const char *options)
{
	snprintf(uri, sizeof(uri), "at:%s%s%s", pts_name, options ? "?" : "", options ? options : "");
}

static void transport_selection_test(void)
{
	printf("transport_selection_test\n");

	assert(ipa_scard_set_transport("pcsc:1") == 0);
	assert(strcmp(ipa_scard_get_transport(), "pcsc:1") == 0);
	assert(ipa_scard_set_transport("at:/dev/null") == 0);
	assert(ipa_scard_set_transport("nosuch:x") < 0);
	/* The refused URI leaves the previous selection in place. */
	assert(strcmp(ipa_scard_get_transport(), "at:/dev/null") == 0);
	/* NULL is the PC/SC default. */
	assert(ipa_scard_set_transport(NULL) == 0);
	assert(strcmp(ipa_scard_get_transport(), "pcsc") == 0);
}

static void open_failure_test(void)
{
	printf("open_failure_test\n");

	assert(ipa_scard_set_transport("at:/nonexistent/tty") == 0);
	assert(ipa_scard_init(0) == NULL);

	/* A device that is not a modem: AT+CSIM=? never answers, so the open fails rather than hanging. */
	assert(ipa_scard_set_transport("at:/dev/null?timeout=300") == 0);
	assert(ipa_scard_init(0) == NULL);

	/* Unknown and malformed options are refused. */
	uri_for("nosuchoption=1");
	assert(ipa_scard_set_transport(uri) == 0);
	assert(ipa_scard_init(0) == NULL);
	uri_for("baud=12345");
	assert(ipa_scard_set_transport(uri) == 0);
	assert(ipa_scard_init(0) == NULL);
	uri_for("reset=sometimes");
	assert(ipa_scard_set_transport(uri) == 0);
	assert(ipa_scard_init(0) == NULL);
}

static void transceive_test(void)
{
	struct ipa_buf *req;
	struct ipa_buf *res;
	void *scard;

	printf("transceive_test\n");

	uri_for("timeout=2000");
	assert(ipa_scard_set_transport(uri) == 0);
	scard = ipa_scard_init(0);
	assert(scard);

	/* A known APDU: the TERMINAL CAPABILITY of the recorded session. */
	req = ipa_buf_alloc_data(10, (uint8_t *)"\x80\xAA\x00\x00\x05\xA9\x03\x84\x01\x01");
	res = ipa_buf_alloc(258);
	assert(ipa_scard_transceive(scard, res, req) == 0);
	assert(res->len == 2 && res->data[0] == 0x90 && res->data[1] == 0x00);

	/* An unsolicited result code arriving before the answer is skipped. */
	modem.urc_before = modem.csim_count + 1;
	res->len = 0;
	assert(ipa_scard_transceive(scard, res, req) == 0);
	assert(res->len == 2 && res->data[0] == 0x90);
	modem.urc_before = 0;

	/* +CME ERROR is a failure, and the next command works again. */
	modem.cme_error_once = true;
	assert(ipa_scard_transceive(scard, res, req) < 0);
	assert(ipa_scard_transceive(scard, res, req) == 0);

	/* An APDU the card does not know still comes back as a status word. */
	IPA_FREE(req);
	req = ipa_buf_alloc_data(5, (uint8_t *)"\x00\xA4\x04\x00\x00");
	assert(ipa_scard_transceive(scard, res, req) == 0);
	assert(res->len == 2 && res->data[0] == 0x6D && res->data[1] == 0x00);

	/* The ATR is not reachable through AT+CSIM. */
	assert(ipa_scard_atr(scard, res) < 0);

	/* Without a reset method the transport says so rather than pretending. */
	assert(ipa_scard_reset(scard) == -ENOTSUP);

	IPA_FREE(req);
	IPA_FREE(res);
	assert(ipa_scard_free(scard) == 0);

	/* The port is free again once the context is released. */
	scard = ipa_scard_init(0);
	assert(scard);
	assert(ipa_scard_free(scard) == 0);
}

static void reset_test(void)
{
	void *scard;

	printf("reset_test\n");

	uri_for("timeout=2000&reset=cfun&resetwait=1000");
	assert(ipa_scard_set_transport(uri) == 0);
	scard = ipa_scard_init(0);
	assert(scard);

	modem.answer_cpin = true;
	assert(ipa_scard_reset(scard) == 0);
	assert(modem.saw_cfun_off && modem.saw_cfun_on);

	/* A modem that never reports the SIM ready does not fail the reset. */
	modem.answer_cpin = false;
	assert(ipa_scard_reset(scard) == 0);

	assert(ipa_scard_free(scard) == 0);
}

/* The whole ES10x link over the transport: TERMINAL CAPABILITY, a channel chosen by the card, SELECT of the
 * ISD-R, and GetEID -- which is what ipa_init() does. */
static void es10x_over_at_test(void)
{
	struct ipa_config cfg = { 0 };
	struct ipa_ctx_info info;
	struct ipa_context *ctx;
	struct ipa_buf *nvstate;
	char eid[2 * IPA_LEN_EID_BYTES + 1];
	size_t i;

	printf("es10x_over_at_test\n");

	uri_for("timeout=2000");
	assert(ipa_scard_set_transport(uri) == 0);

	cfg.euicc_channel = IPA_EUICC_CHANNEL_AUTO;
	cfg.esipa_binding = IPA_ESIPA_BINDING_ASN1;
	ctx = ipa_new_ctx(&cfg, NULL);
	assert(ctx);

#ifdef IPA_HAVE_ESIPA_ASN1
	assert(ipa_init(ctx) == 0);

	assert(ipa_get_ctx_info(ctx, &info) == 0);
	assert(info.eid_valid);
	for (i = 0; i < sizeof(info.eid); i++)
		sprintf(eid + 2 * i, "%02X", info.eid[i]);
	printf("  EID read over AT+CSIM: %s\n", eid);
	assert(strcmp(eid, TEST_EID) == 0);

	/* The card handed out channel 5, so the commands used the class bytes of channels 4 to 19. The script
	 * would have answered 6D00 to anything else, and GetEID would have failed. */
	ipa_close(ctx);
#else
	(void)info;
	(void)eid;
	(void)i;
	printf("  built without the ASN.1 binding, skipping\n");
#endif

	nvstate = ipa_free_ctx(ctx);
	IPA_FREE(nvstate);
}

int main(void)
{
	/* The transport logs every command at debug level; keep the test output readable. */
	ipa_log_set_level_all(LERROR);

	modem_start();

	transport_selection_test();
	open_failure_test();
	transceive_test();
	reset_test();
	es10x_over_at_test();

	modem_stop();

	printf("scard_at_test: all tests passed\n");
	return 0;
}
