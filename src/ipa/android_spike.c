/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * On-hardware eUICC transport spike (ANDROID_PORT_PLAN.md, Phase 1 gating risk).
 *
 * The core drives its own 61xx GET RESPONSE loop (libipa/euicc.c) and the
 * Android transport (scard_android.c + EuiccChannel) passes framework responses
 * through verbatim.  Whether that composition works end to end depends entirely
 * on a modem/RIL behaviour we cannot observe on this build machine:
 *
 *   - Does iccTransmitApduLogicalChannel expose ISO 61xx chaining to us (so the
 *     core's GET RESPONSE loop is required and functional), or does it
 *     auto-assemble the full body and return SW=9000 (so the core loop never
 *     fires and must simply tolerate a long single response)?
 *   - Does a bare GET RESPONSE (00 C0 00 00 Le) even survive the logical-channel
 *     transport, or does the framework swallow / rewrite it?
 *
 * This file answers those questions empirically.  It drives the *real*
 * libipacore transport (ipa_scard_* over JNI to the same EuiccChannel the
 * production backend uses) with a handful of well-known ES10x commands, records
 * every raw APDU exchange, and classifies the modem's chaining behaviour.  It
 * deliberately works at the public scard.h level -- no ipa_context, no ES10x
 * codec -- so it exercises exactly the byte path under test and nothing else.
 * The result is returned as a human-readable report for on-screen display; the
 * core logger writes to stderr, which is invisible in an APK.
 *
 * This whole translation unit is Android-only (built into libipacore.so behind
 * IPA_TARGET_ANDROID) and is a diagnostic, not part of the production flow.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <jni.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/mem.h>
#include <onomondo/ipa/scard.h>

/* ------------------------------------------------------------------------- */
/* Growable text report                                                      */
/* ------------------------------------------------------------------------- */

struct report {
	char  *buf;
	size_t len;
	size_t cap;
};

static void report_reserve(struct report *r, size_t extra)
{
	if (r->len + extra + 1 <= r->cap)
		return;
	while (r->len + extra + 1 > r->cap)
		r->cap = r->cap ? r->cap * 2 : 4096;
	r->buf = IPA_REALLOC(r->buf, r->cap);
}

static void report_printf(struct report *r, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;

	report_reserve(r, (size_t) n);
	va_start(ap, fmt);
	vsnprintf(r->buf + r->len, (size_t) n + 1, fmt, ap);
	va_end(ap);
	r->len += (size_t) n;
}

static void report_hex(struct report *r, const uint8_t *data, size_t len)
{
	static const char hexd[] = "0123456789ABCDEF";
	size_t i;

	report_reserve(r, len * 3);
	for (i = 0; i < len; i++) {
		r->buf[r->len++] = hexd[(data[i] >> 4) & 0xF];
		r->buf[r->len++] = hexd[data[i] & 0xF];
		if (i + 1 < len)
			r->buf[r->len++] = ' ';
	}
	r->buf[r->len] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Raw APDU exchange over the production scard transport                     */
/* ------------------------------------------------------------------------- */

#define SPIKE_RX_MAX 2048

/* Transmit one raw APDU and capture the raw response (payload + SW).
 * Returns 0 on success, negative errno on transport failure. */
static int spike_apdu(void *scard, struct report *r, const char *label,
		      const uint8_t *apdu, size_t apdu_len,
		      uint8_t *resp, size_t *resp_len, uint16_t *sw)
{
	struct ipa_buf *req;
	struct ipa_buf *res;
	int rc;

	*resp_len = 0;
	*sw = 0;

	report_printf(r, "  -> %s\n     APDU: ", label);
	report_hex(r, apdu, apdu_len);
	report_printf(r, "\n");

	req = ipa_buf_alloc_and_cpy(apdu, apdu_len);
	res = ipa_buf_alloc(SPIKE_RX_MAX);

	rc = ipa_scard_transceive(scard, res, req);
	if (rc < 0) {
		report_printf(r, "     transceive FAILED (rc=%d)\n", rc);
		ipa_buf_free(req);
		ipa_buf_free(res);
		return rc;
	}

	if (res->len < 2) {
		report_printf(r, "     response too short (%zu bytes, no SW)\n", res->len);
		ipa_buf_free(req);
		ipa_buf_free(res);
		return -1;
	}

	*sw = (uint16_t) ((res->data[res->len - 2] << 8) | res->data[res->len - 1]);
	*resp_len = res->len - 2;
	if (*resp_len > 0)
		memcpy(resp, res->data, *resp_len);

	report_printf(r, "     <- %zu body byte(s), SW=%04X\n", *resp_len, *sw);
	if (*resp_len) {
		report_printf(r, "     body: ");
		report_hex(r, resp, *resp_len);
		report_printf(r, "\n");
	}

	ipa_buf_free(req);
	ipa_buf_free(res);
	return 0;
}

/* One ES10x probe: send the STORE DATA command, then -- exactly as euicc.c does
 * -- follow any 61xx chaining with GET RESPONSE.  Records the observed modem
 * behaviour into the report and back into *auto_assembled / *exposed_chaining
 * so the caller can render a verdict. */
static void spike_es10x(void *scard, struct report *r, const char *name,
			const uint8_t *payload, size_t payload_len,
			bool *exposed_chaining, bool *auto_assembled, bool *get_response_ok)
{
	uint8_t apdu[5 + 255];
	uint8_t resp[SPIKE_RX_MAX];
	size_t  resp_len;
	uint16_t sw;
	size_t  assembled = 0;
	int rounds = 0;

	report_printf(r, "\n== ES10x probe: %s ==\n", name);

	if (payload_len > 255) {
		report_printf(r, "  (payload too large for single-block spike, skipping)\n");
		return;
	}

	/* STORE DATA, last block, block 0 -- mirrors euicc.c:send_es10x_block for
	 * the single-block case (CLA 80, INS E2, P1 91, P2 00, no Le). */
	apdu[0] = 0x80;
	apdu[1] = 0xE2;
	apdu[2] = 0x91;
	apdu[3] = 0x00;
	apdu[4] = (uint8_t) payload_len;
	memcpy(apdu + 5, payload, payload_len);

	if (spike_apdu(scard, r, "STORE DATA", apdu, 5 + payload_len, resp, &resp_len, &sw) < 0)
		return;

	assembled = resp_len;

	if ((sw & 0xFF00) == 0x9000) {
		if (resp_len > 256) {
			report_printf(r,
				"  VERDICT: modem AUTO-ASSEMBLED %zu bytes and returned SW=9000 on the\n"
				"           triggering command. The core's 61xx GET RESPONSE loop will\n"
				"           NOT fire -- verify euicc.c tolerates a long single response.\n",
				resp_len);
			*auto_assembled = true;
		} else {
			report_printf(r,
				"  VERDICT: complete in one exchange (SW=9000, %zu bytes). Response fit\n"
				"           in a single APDU; chaining not exercised by this command.\n",
				resp_len);
		}
		return;
	}

	if ((sw & 0xFF00) == 0x6100) {
		report_printf(r,
			"  Modem EXPOSED ISO 61xx chaining (SW=61%02X). Following with GET RESPONSE,\n"
			"  exactly as euicc.c:recv_es10x_block does...\n", sw & 0xFF);
		*exposed_chaining = true;

		/* GET RESPONSE loop: 00 C0 00 00 Le, until 90 00 (or a non-61xx SW). */
		while ((sw & 0xFF00) == 0x6100 && rounds++ < 64) {
			uint8_t le = (uint8_t) (sw & 0xFF); /* 0 == 256 */
			uint8_t gr[5] = { 0x00, 0xC0, 0x00, 0x00, le };

			if (spike_apdu(scard, r, "GET RESPONSE", gr, sizeof(gr), resp, &resp_len, &sw) < 0)
				return;

			/* 6Cxx: wrong Le, retry with the length the card wants. */
			if ((sw & 0xFF00) == 0x6C00) {
				gr[4] = (uint8_t) (sw & 0xFF);
				if (spike_apdu(scard, r, "GET RESPONSE (Le corrected)", gr, sizeof(gr),
					       resp, &resp_len, &sw) < 0)
					return;
			}

			assembled += resp_len;
		}

		if ((sw & 0xFF00) == 0x9000) {
			report_printf(r,
				"  VERDICT: GET RESPONSE chaining WORKS over the logical channel.\n"
				"           Assembled %zu bytes, final SW=9000. The core's loop is viable.\n",
				assembled);
			*get_response_ok = true;
		} else {
			report_printf(r,
				"  VERDICT: chaining did NOT complete cleanly (final SW=%04X after %d\n"
				"           GET RESPONSE round(s)). The transport may swallow or rewrite\n"
				"           GET RESPONSE on the logical channel -- inspect the exchange above.\n",
				sw, rounds);
		}
		return;
	}

	report_printf(r, "  VERDICT: unexpected SW=%04X -- see raw exchange above.\n", sw);
}

/* ------------------------------------------------------------------------- */
/* JNI entry point                                                           */
/* ------------------------------------------------------------------------- */

/* GetEID (ES10c) request: short, single-response -- proves the channel is live. */
static const uint8_t es10c_get_eid[]      = { 0xBF, 0x3E, 0x03, 0x5C, 0x01, 0x5A };
/* GetEUICCInfo1 (ES10b): moderate -- may or may not need chaining. */
static const uint8_t es10b_get_info1[]    = { 0xBF, 0x20, 0x00 };
/* GetEUICCInfo2 (ES10b): large (typically > 256 bytes) -- the real 61xx trigger. */
static const uint8_t es10b_get_info2[]    = { 0xBF, 0x22, 0x00 };

/*
 * com.onomondo.ipa.spike.EuiccSpike.nativeRunSpike(int slot): open the ISD-R
 * channel via the production transport and run the ES10x chaining probes on the
 * given SIM slot.  Returns the full report as a String for on-screen display.
 * An EuiccChannel instance must already be registered (its init{} calls
 * nativeRegister) before this is invoked.
 */
JNIEXPORT jstring JNICALL
Java_com_onomondo_ipa_spike_EuiccSpike_nativeRunSpike(JNIEnv *env, jobject thiz, jint slot)
{
	struct report r = { 0 };
	void *scard;
	bool exposed_chaining = false;
	bool auto_assembled = false;
	bool get_response_ok = false;
	jstring out;

	(void) thiz;

	report_printf(&r, "eUICC transport spike (ANDROID_PORT_PLAN.md, Phase 1)\n");
	report_printf(&r, "libipacore built for ABI: "
#if defined(__aarch64__)
		"arm64-v8a"
#elif defined(__arm__)
		"armeabi-v7a"
#elif defined(__x86_64__)
		"x86_64"
#elif defined(__i386__)
		"x86"
#else
		"unknown"
#endif
		"\n");
	report_printf(&r, "SIM slot under test: %d\n", (int) slot);
	report_printf(&r, "-------------------------------------------------------------\n");

	/* Opens the logical channel to the ISD-R via EuiccChannel.openChannel
	 * (MANAGE CHANNEL + ISD-R SELECT owned by the framework). */
	scard = ipa_scard_init((unsigned int) slot);
	if (!scard) {
		report_printf(&r,
			"\nFAILED to open the ISD-R logical channel.\n"
			"Likely causes: the app lacks carrier privileges / is not system-signed\n"
			"(iccOpenLogicalChannel needs MODIFY_PHONE_STATE), no eUICC in this slot,\n"
			"or EuiccChannel was not registered. See logcat (tag IpaEuiccChannel).\n"
			"If access is the blocker, the OMAPI fallback (ANDROID_PORT_PLAN.md) is the\n"
			"next thing to try -- it is a Kotlin-only swap behind the same C backend.\n");
		goto done;
	}

	report_printf(&r, "ISD-R channel opened. scard_manages_channel = %s\n",
		      ipa_scard_manages_channel(scard) ? "true (framework owns channel)" : "false");

	spike_es10x(scard, &r, "GetEID (ES10c, short)", es10c_get_eid, sizeof(es10c_get_eid),
		    &exposed_chaining, &auto_assembled, &get_response_ok);
	spike_es10x(scard, &r, "GetEUICCInfo1 (ES10b)", es10b_get_info1, sizeof(es10b_get_info1),
		    &exposed_chaining, &auto_assembled, &get_response_ok);
	spike_es10x(scard, &r, "GetEUICCInfo2 (ES10b, large)", es10b_get_info2, sizeof(es10b_get_info2),
		    &exposed_chaining, &auto_assembled, &get_response_ok);

	report_printf(&r, "\n=============================================================\n");
	report_printf(&r, "SUMMARY\n");
	if (exposed_chaining)
		report_printf(&r, "  * Modem exposes 61xx chaining; core GET RESPONSE loop %s.\n",
			      get_response_ok ? "assembles correctly" : "did NOT complete -- investigate");
	if (auto_assembled)
		report_printf(&r, "  * Modem auto-assembles long responses (SW=9000); core loop is a no-op\n"
				  "    -- confirm euicc.c accepts a >256-byte single response.\n");
	if (!exposed_chaining && !auto_assembled)
		report_printf(&r, "  * No command exercised >256-byte chaining. Try with a profile-bearing\n"
				  "    eUICC or an ES10x command with a larger response.\n");
	report_printf(&r,
		"\nNOT covered by this passive spike: proactive REFRESH (SW=91xx) and the\n"
		"basic-channel FETCH / TERMINAL RESPONSE STK path -- those need a real profile\n"
		"enable/disable and are the remaining on-hardware item after this.\n");

	ipa_scard_free(scard);

done:
	out = (*env)->NewStringUTF(env, r.buf ? r.buf : "spike produced no output");
	if (r.buf)
		IPA_FREE(r.buf);
	return out;
}
