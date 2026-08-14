/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * JNI bridge for the Android eUICC scard backend (scard_android.c).
 *
 * Caches the JavaVM in JNI_OnLoad, provides the thread attach/detach helper the
 * backend uses (the IPA poll loop runs on a native thread), and exposes the
 * register/unregister entry points that com.onomondo.ipa.EuiccChannel calls to
 * hand its instance (and resolved method IDs) down to native code.  See
 * ANDROID_PORT_PLAN.md, Phase 1.
 */

#include <stddef.h>
#include <jni.h>
#include <onomondo/ipa/log.h>
#include "scard_android.h"

struct android_euicc_jni g_euicc_jni;

jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
	(void) reserved;
	g_euicc_jni.vm = vm;
	return JNI_VERSION_1_6;
}

JNIEnv *ipa_android_jni_env(bool *did_attach)
{
	JNIEnv *env = NULL;
	jint r;

	*did_attach = false;
	if (!g_euicc_jni.vm)
		return NULL;

	r = (*g_euicc_jni.vm)->GetEnv(g_euicc_jni.vm, (void **) &env, JNI_VERSION_1_6);
	if (r == JNI_EDETACHED) {
		if ((*g_euicc_jni.vm)->AttachCurrentThread(g_euicc_jni.vm, &env, NULL) != 0)
			return NULL;
		*did_attach = true;
	} else if (r != JNI_OK) {
		return NULL;
	}
	return env;
}

void ipa_android_jni_detach(bool did_attach)
{
	if (did_attach && g_euicc_jni.vm)
		(*g_euicc_jni.vm)->DetachCurrentThread(g_euicc_jni.vm);
}

/* com.onomondo.ipa.EuiccChannel.nativeRegister(): store a global ref to the
 * EuiccChannel instance and resolve the method IDs the backend calls. */
JNIEXPORT void JNICALL
Java_com_onomondo_ipa_EuiccChannel_nativeRegister(JNIEnv *env, jobject thiz)
{
	jclass cls;

	if (g_euicc_jni.channel_obj)
		(*env)->DeleteGlobalRef(env, g_euicc_jni.channel_obj);
	g_euicc_jni.channel_obj = (*env)->NewGlobalRef(env, thiz);

	cls = (*env)->GetObjectClass(env, thiz);
	g_euicc_jni.open_mid     = (*env)->GetMethodID(env, cls, "openChannel", "(I[B)I");
	g_euicc_jni.transmit_mid = (*env)->GetMethodID(env, cls, "transmit", "(I[B)[B");
	g_euicc_jni.close_mid    = (*env)->GetMethodID(env, cls, "closeChannel", "(I)Z");
	(*env)->DeleteLocalRef(env, cls);

	if (!g_euicc_jni.open_mid || !g_euicc_jni.transmit_mid || !g_euicc_jni.close_mid)
		IPA_LOGP(SSCARD, LERROR, "Android eUICC: failed to resolve EuiccChannel method IDs\n");
	else
		IPA_LOGP(SSCARD, LINFO, "Android eUICC: EuiccChannel registered\n");
}

/* com.onomondo.ipa.EuiccChannel.nativeUnregister(): drop the global ref. */
JNIEXPORT void JNICALL
Java_com_onomondo_ipa_EuiccChannel_nativeUnregister(JNIEnv *env, jobject thiz)
{
	(void) thiz;
	if (g_euicc_jni.channel_obj) {
		(*env)->DeleteGlobalRef(env, g_euicc_jni.channel_obj);
		g_euicc_jni.channel_obj = NULL;
	}
	g_euicc_jni.open_mid = NULL;
	g_euicc_jni.transmit_mid = NULL;
	g_euicc_jni.close_mid = NULL;
}
