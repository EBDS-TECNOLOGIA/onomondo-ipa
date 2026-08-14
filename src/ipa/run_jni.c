/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * JNI bridge for the IPAd run loop (ANDROID_PORT_PLAN.md, Phase 4).
 *
 * This is the entry point for the app-hosted daemon: a foreground Service
 * calls nativeRun() from a worker thread, which blocks for the whole poll
 * cycle, and nativeStop() from any other thread to bring it down gracefully.
 *
 * Why the daemon lives in the app rather than in an init service: the
 * telephony transport reaches the ISD-R only from a process that holds
 * MODIFY_PHONE_STATE *and* is the device's LPA (Phase-1 finding).  Both are
 * properties of an Android package, so the process driving the eUICC has to be
 * the app.  See contrib/android/README.md.
 *
 * Threading: nativeRun() is called on an already-attached Java thread, so the
 * eUICC backend's upcalls into com.onomondo.ipa.EuiccChannel find a JNIEnv
 * without attaching.  It must not be called on the main thread -- it does not
 * return until the poll cycle ends.
 */

#include <errno.h>
#include <jni.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/config_json.h>

/* com.onomondo.ipa.NativeBridge.nativeRun(String): run the IPAd to completion
 * using the given JSON configuration file.  Returns 0 on success, or a
 * negative error (-EBUSY when a run is already in progress). */
JNIEXPORT jint JNICALL
Java_com_onomondo_ipa_NativeBridge_nativeRun(JNIEnv *env, jobject thiz, jstring config_path)
{
	const char *path_utf;
	jint rc;

	(void) thiz;

	if (!config_path) {
		IPA_LOGP(SMAIN, LERROR, "nativeRun: no configuration path given\n");
		return -EINVAL;
	}

	path_utf = (*env)->GetStringUTFChars(env, config_path, NULL);
	if (!path_utf)
		return -ENOMEM;

	/* Blocks here for the whole poll cycle.  The string is pinned across the
	 * call, which is fine: it is a handful of bytes and releasing it early
	 * would mean copying it first. */
	rc = ipa_run_from_config(path_utf);

	(*env)->ReleaseStringUTFChars(env, config_path, path_utf);
	return rc;
}

/* com.onomondo.ipa.NativeBridge.nativeStop(): ask a running nativeRun() to
 * leave its poll loop.  Returns immediately; nativeRun() returns once the
 * in-flight eIM request and eUICC exchange have finished, so a caller that
 * needs a hard deadline should also time out its worker thread. */
JNIEXPORT void JNICALL
Java_com_onomondo_ipa_NativeBridge_nativeStop(JNIEnv *env, jobject thiz)
{
	(void) env;
	(void) thiz;

	ipa_run_stop();
}
