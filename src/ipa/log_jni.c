/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * JNI bridge for the log sinks (ANDROID_PORT_PLAN.md, Phase 3).
 *
 * The APK tails the log live while the IPAd runs: it installs the ring-buffer
 * sink and polls nativeLogDrain() from its UI, while the IPAd writes into the
 * ring from the poll loop's own native thread.  The daemon front-end uses the
 * rotating-file sink instead, which it configures from the JSON file and never
 * needs to reach through JNI -- but the same entry points are exposed here so
 * an APK that wants a log file as well as the live view can ask for one.
 *
 * Draining is destructive: each call returns the text buffered since the last
 * call, so the UI appends what it gets rather than re-rendering everything.
 */

#include <stdlib.h>
#include <jni.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/log_sink.h>

/* Upper bound on one drain, so a UI that has not polled for a while cannot ask
 * for an unbounded allocation.  Whatever is left over stays in the ring and
 * comes back on the next call. */
#define DRAIN_CHUNK_MAX (64 * 1024)

/* com.onomondo.ipa.NativeBridge.nativeLogRingInit(int): install the ring sink.
 * capacityBytes <= 0 selects the default capacity. */
JNIEXPORT jint JNICALL
Java_com_onomondo_ipa_NativeBridge_nativeLogRingInit(JNIEnv *env, jobject thiz, jint capacity_bytes)
{
	(void) env;
	(void) thiz;

	return ipa_log_ring_sink_init(capacity_bytes > 0 ? (size_t) capacity_bytes : 0);
}

/* com.onomondo.ipa.NativeBridge.nativeLogRingFree(): drop the ring sink and go
 * back to logging via stderr (which on Android lands in logcat). */
JNIEXPORT void JNICALL
Java_com_onomondo_ipa_NativeBridge_nativeLogRingFree(JNIEnv *env, jobject thiz)
{
	(void) env;
	(void) thiz;

	ipa_log_ring_sink_free();
}

/* com.onomondo.ipa.NativeBridge.nativeLogDrain(): return the log text buffered
 * since the previous call, or null when there is nothing new.  Returning null
 * rather than an empty string lets the UI skip the append entirely on an idle
 * poll. */
JNIEXPORT jstring JNICALL
Java_com_onomondo_ipa_NativeBridge_nativeLogDrain(JNIEnv *env, jobject thiz)
{
	jstring result;
	size_t avail;
	size_t len;
	char *buf;

	(void) thiz;

	avail = ipa_log_ring_sink_avail();
	if (avail == 0)
		return NULL;

	if (avail > DRAIN_CHUNK_MAX)
		avail = DRAIN_CHUNK_MAX;

	buf = malloc(avail + 1);
	if (!buf)
		return NULL;

	len = ipa_log_ring_sink_read(buf, avail + 1);
	if (len == 0) {
		free(buf);
		return NULL;
	}

	/* The ring holds log records, which are ASCII, so this is valid
	 * modified UTF-8 as NewStringUTF requires. */
	result = (*env)->NewStringUTF(env, buf);
	free(buf);
	return result;
}

/* com.onomondo.ipa.NativeBridge.nativeLogFileInit(String, long, int): install
 * the rotating-file sink.  Returns 0 on success, a negative errno on failure. */
JNIEXPORT jint JNICALL
Java_com_onomondo_ipa_NativeBridge_nativeLogFileInit(JNIEnv *env, jobject thiz, jstring path,
						     jlong max_size_bytes, jint max_files)
{
	const char *path_utf;
	jint rc;

	(void) thiz;

	if (!path)
		return -1;

	path_utf = (*env)->GetStringUTFChars(env, path, NULL);
	if (!path_utf)
		return -1;

	rc = ipa_log_file_sink_init(path_utf, max_size_bytes > 0 ? (size_t) max_size_bytes : 0,
				    max_files > 0 ? (unsigned int) max_files : 1);

	(*env)->ReleaseStringUTFChars(env, path, path_utf);
	return rc;
}

/* com.onomondo.ipa.NativeBridge.nativeLogFileFree(): close the log file. */
JNIEXPORT void JNICALL
Java_com_onomondo_ipa_NativeBridge_nativeLogFileFree(JNIEnv *env, jobject thiz)
{
	(void) env;
	(void) thiz;

	ipa_log_file_sink_free();
}
