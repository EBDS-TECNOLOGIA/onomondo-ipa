/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * Internal glue shared between the Android eUICC scard backend
 * (scard_android.c) and the JNI bridge (scard_jni.c).  See
 * ANDROID_PORT_PLAN.md, Phase 1.
 */

#pragma once

#include <jni.h>
#include <stdbool.h>

/* JNI handles for the Java-side eUICC transport (com.onomondo.ipa.EuiccChannel).
 * Populated by EuiccChannel.nativeRegister() (scard_jni.c) before ipa_init()
 * opens the card, and consumed by the scard backend on every APDU. */
struct android_euicc_jni {
	JavaVM   *vm;           /* cached in JNI_OnLoad */
	jobject   channel_obj;  /* global ref to the EuiccChannel, or NULL */
	jmethodID open_mid;     /* int  openChannel(int slotIndex, byte[] aid) */
	jmethodID transmit_mid; /* byte[] transmit(int channel, byte[] apdu)   */
	jmethodID close_mid;    /* boolean closeChannel(int channel)           */
};

extern struct android_euicc_jni g_euicc_jni;

/* Attach the calling thread to the JVM (the IPA poll loop runs on a native
 * thread, so this may be an unattached thread).  Returns a usable JNIEnv* and
 * sets *did_attach if a matching ipa_android_jni_detach() is required, or NULL
 * if no JVM/EuiccChannel has been registered. */
JNIEnv *ipa_android_jni_env(bool *did_attach);
void ipa_android_jni_detach(bool did_attach);
