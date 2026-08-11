/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & Iapyx Informatica Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * Android eUICC (scard) backend.
 *
 * Phase 0 (NDK build) placeholder: this translation unit exists so that
 * libipacore.so links and exposes the full portable core + net + crypto with
 * *no eUICC yet* -- exactly the Phase-0 deliverable in ANDROID_PORT_PLAN.md.
 * It implements the 5-function smartcard contract from <onomondo/ipa/scard.h>
 * (the seam the core is written against) as not-yet-implemented stubs.
 *
 * Phase 1 replaces the bodies below with the real JNI bridge to the Android
 * telephony stack -- TelephonyManager iccOpenLogicalChannel /
 * iccTransmitApduLogicalChannel / iccCloseLogicalChannel (recommended), or
 * OMAPI as a fallback -- per ANDROID_PORT_PLAN.md "Phase 1 -- Android scard
 * backend".  Phase 1 also adds the scard_manages_channel capability so the
 * core stops doing MANAGE CHANNEL / ISD-R SELECT itself when the framework
 * owns the channel; nothing here needs to change for that.
 */

#include <stddef.h>
#include <errno.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/scard.h>
#include <onomondo/ipa/log.h>

#define ANDROID_SCARD_TODO(func) \
	IPA_LOGP(SSCARD, LERROR, \
		 "%s: Android eUICC backend not implemented yet (ANDROID_PORT_PLAN.md Phase 1)\n", \
		 func)

/*! Open the eUICC channel (Phase 1: JNI logical channel to the ISD-R AID).
 *  \param[in] reader_num maps to the SIM slot index on Android.
 *  \returns NULL -- not implemented yet. */
void *ipa_scard_init(unsigned int reader_num)
{
	(void)reader_num;
	ANDROID_SCARD_TODO(__func__);
	return NULL;
}

/*! Transceive an APDU (Phase 1: marshal to the framework channel over JNI).
 *  \returns -ENOSYS -- not implemented yet. */
int ipa_scard_transceive(void *scard_ctx, struct ipa_buf *res, const struct ipa_buf *req)
{
	(void)scard_ctx;
	(void)res;
	(void)req;
	ANDROID_SCARD_TODO(__func__);
	return -ENOSYS;
}

/*! Reset the card -- never called by the core; stub returns success. */
int ipa_scard_reset(void *scard_ctx)
{
	(void)scard_ctx;
	return 0;
}

/*! Read the ATR -- never called by the core; stub returns not-implemented. */
int ipa_scard_atr(void *scard_ctx, struct ipa_buf *atr)
{
	(void)scard_ctx;
	(void)atr;
	ANDROID_SCARD_TODO(__func__);
	return -ENOSYS;
}

/*! Close the eUICC channel (Phase 1: close the logical channel, drop JNI
 *  globals).  Safe to call with a NULL context. */
int ipa_scard_free(void *scard_ctx)
{
	(void)scard_ctx;
	return 0;
}
