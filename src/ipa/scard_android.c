/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * Android eUICC (scard) backend.
 *
 * Implements the 5-function smartcard contract from <onomondo/ipa/scard.h> on
 * top of the Android telephony stack, via a Java helper
 * (com.onomondo.ipa.EuiccChannel, see src/ipa/android/EuiccChannel.kt) reached
 * over JNI.  The framework owns the logical channel: iccOpenLogicalChannel
 * performs MANAGE CHANNEL + the ISD-R SELECT and iccTransmitApduLogicalChannel
 * rewrites the CLA channel bits, so ipa_scard_manages_channel() returns true
 * and the core (euicc.c) skips its own channel management.  See
 * ANDROID_PORT_PLAN.md, Phase 1.
 *
 * APDU decomposition (into CLA/INS/P1/P2/P3/data for
 * iccTransmitApduLogicalChannel) is done Java-side in EuiccChannel.transmit();
 * this file marshals only raw APDU bytes in and raw response bytes (incl. SW)
 * out, which keeps the C backend identical for an OMAPI transport (raw
 * Channel.transmit(byte[])) too.
 */

#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <jni.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/mem.h>
#include <onomondo/ipa/scard.h>
#include <onomondo/ipa/log.h>
#include "scard_android.h"

/* ISD-R application identifier (GSMA SGP.22 §2.2.3); the framework SELECTs it
 * when the logical channel is opened.  Mirrors the AID in euicc.c:select_isd_r. */
static const uint8_t aid_isd_r[] =
    { 0xA0, 0x00, 0x00, 0x05, 0x59, 0x10, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x89, 0x00, 0x00, 0x01, 0x00 };

struct android_scard_ctx {
	unsigned int slot;    /* SIM slot index (maps from reader_num) */
	int channel;          /* framework-assigned logical channel number */
};

/* Log and clear a pending Java exception; returns true if one was pending. */
static bool clear_jni_exception(JNIEnv *env, const char *what)
{
	if (!(*env)->ExceptionCheck(env))
		return false;
	IPA_LOGP(SSCARD, LERROR, "Android eUICC: Java exception during %s\n", what);
	(*env)->ExceptionDescribe(env);
	(*env)->ExceptionClear(env);
	return true;
}

/*! Open the eUICC logical channel to the ISD-R via the Java transport.
 *  \param[in] reader_num SIM slot index.
 *  \returns scard context on success, NULL on failure. */
void *ipa_scard_init(unsigned int reader_num)
{
	struct android_scard_ctx *ctx;
	bool attached;
	JNIEnv *env;
	jbyteArray aid;
	jint channel;

	env = ipa_android_jni_env(&attached);
	if (!env) {
		IPA_LOGP(SSCARD, LERROR, "Android eUICC: no JVM registered (EuiccChannel not installed?)\n");
		return NULL;
	}
	if (!g_euicc_jni.channel_obj || !g_euicc_jni.open_mid) {
		IPA_LOGP(SSCARD, LERROR, "Android eUICC: EuiccChannel not registered\n");
		ipa_android_jni_detach(attached);
		return NULL;
	}

	aid = (*env)->NewByteArray(env, (jsize) sizeof(aid_isd_r));
	if (!aid) {
		clear_jni_exception(env, "NewByteArray(aid)");
		ipa_android_jni_detach(attached);
		return NULL;
	}
	(*env)->SetByteArrayRegion(env, aid, 0, (jsize) sizeof(aid_isd_r), (const jbyte *) aid_isd_r);

	channel = (*env)->CallIntMethod(env, g_euicc_jni.channel_obj, g_euicc_jni.open_mid, (jint) reader_num, aid);
	(*env)->DeleteLocalRef(env, aid);

	if (clear_jni_exception(env, "openChannel") || channel < 0) {
		IPA_LOGP(SSCARD, LERROR, "Android eUICC: failed to open ISD-R channel on slot %u\n", reader_num);
		ipa_android_jni_detach(attached);
		return NULL;
	}

	ctx = IPA_ALLOC_ZERO(struct android_scard_ctx);
	ctx->slot = reader_num;
	ctx->channel = (int) channel;

	IPA_LOGP(SSCARD, LINFO, "Android eUICC: ISD-R channel %d opened on slot %u\n", ctx->channel, reader_num);
	ipa_android_jni_detach(attached);
	return ctx;
}

/*! Transceive one APDU through the framework logical channel.
 *  \returns 0 on success, negative errno on failure. */
int ipa_scard_transceive(void *scard_ctx, struct ipa_buf *res, const struct ipa_buf *req)
{
	struct android_scard_ctx *ctx = scard_ctx;
	bool attached;
	JNIEnv *env;
	jbyteArray apdu;
	jbyteArray resp;
	jsize n;
	int rc = -EIO;

	if (!ctx || !res || !req)
		return -EINVAL;

	env = ipa_android_jni_env(&attached);
	if (!env || !g_euicc_jni.channel_obj || !g_euicc_jni.transmit_mid) {
		IPA_LOGP(SSCARD, LERROR, "Android eUICC: transceive with no transport\n");
		ipa_android_jni_detach(attached);
		return -EIO;
	}

	IPA_LOGP(SSCARD, LDEBUG, "Android eUICC channel %d TX:\n", ctx->channel);
	ipa_buf_hexdump_multiline(req, 64, 1, SSCARD, LINFO);

	apdu = (*env)->NewByteArray(env, (jsize) req->len);
	if (!apdu) {
		clear_jni_exception(env, "NewByteArray(apdu)");
		ipa_android_jni_detach(attached);
		return -EIO;
	}
	(*env)->SetByteArrayRegion(env, apdu, 0, (jsize) req->len, (const jbyte *) req->data);

	resp = (jbyteArray) (*env)->CallObjectMethod(env, g_euicc_jni.channel_obj,
						     g_euicc_jni.transmit_mid, (jint) ctx->channel, apdu);
	(*env)->DeleteLocalRef(env, apdu);

	if (clear_jni_exception(env, "transmit") || !resp) {
		IPA_LOGP(SSCARD, LERROR, "Android eUICC: transmit failed on channel %d\n", ctx->channel);
		if (resp)
			(*env)->DeleteLocalRef(env, resp);
		ipa_android_jni_detach(attached);
		return -EIO;
	}

	n = (*env)->GetArrayLength(env, resp);
	if ((size_t) n > res->data_len) {
		IPA_LOGP(SSCARD, LERROR,
			 "Android eUICC: response (%d bytes) exceeds buffer (%zu), clipping\n", (int) n, res->data_len);
		n = (jsize) res->data_len;
	}
	(*env)->GetByteArrayRegion(env, resp, 0, n, (jbyte *) res->data);
	(*env)->DeleteLocalRef(env, resp);
	res->len = (size_t) n;

	IPA_LOGP(SSCARD, LDEBUG, "Android eUICC channel %d RX:\n", ctx->channel);
	ipa_buf_hexdump_multiline(res, 64, 1, SSCARD, LINFO);

	rc = 0;
	ipa_android_jni_detach(attached);
	return rc;
}

/*! Reset — never called by the core; the modem owns card power. */
int ipa_scard_reset(void *scard_ctx)
{
	(void) scard_ctx;
	return 0;
}

/*! ATR — never called by the core; not available through this transport. */
int ipa_scard_atr(void *scard_ctx, struct ipa_buf *atr)
{
	(void) scard_ctx;
	if (atr)
		atr->len = 0;
	return 0;
}

/*! The framework manages the logical channel (MANAGE CHANNEL / ISD-R SELECT /
 *  CLA channel bits), so the core must not. */
bool ipa_scard_manages_channel(void *scard_ctx)
{
	(void) scard_ctx;
	return true;
}

/*! Close the logical channel and free the context. */
int ipa_scard_free(void *scard_ctx)
{
	struct android_scard_ctx *ctx = scard_ctx;
	bool attached;
	JNIEnv *env;

	if (!ctx)
		return 0;

	env = ipa_android_jni_env(&attached);
	if (env && g_euicc_jni.channel_obj && g_euicc_jni.close_mid) {
		(*env)->CallBooleanMethod(env, g_euicc_jni.channel_obj, g_euicc_jni.close_mid, (jint) ctx->channel);
		clear_jni_exception(env, "closeChannel");
		IPA_LOGP(SSCARD, LINFO, "Android eUICC: channel %d closed\n", ctx->channel);
	}
	ipa_android_jni_detach(attached);

	IPA_FREE(ctx);
	return 0;
}
