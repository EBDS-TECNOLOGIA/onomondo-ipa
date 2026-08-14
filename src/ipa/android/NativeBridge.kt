/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

package com.onomondo.ipa

/**
 * Kotlin face of the native IPAd (libipacore.so), Phase 4/5 of
 * ANDROID_PORT_PLAN.md.
 *
 * Pairs with `src/ipa/run_jni.c` and `src/ipa/log_jni.c`. It lives in the core
 * tree next to [EuiccChannel] because the JNI symbol names encode this exact
 * package *and* method name: `Java_com_onomondo_ipa_NativeBridge_nativeRun`
 * and friends. Renaming this class, moving it to another package, or renaming
 * one of the `native*` declarations below silently breaks the linkage — you
 * get `UnsatisfiedLinkError` on the first call, not at load time. The public
 * wrappers exist precisely so the external names can stay pinned to the C side
 * while callers get readable ones.
 *
 * Threading contract:
 *  - [run] blocks for the whole poll cycle. Call it from a worker thread; the
 *    eUICC transport's upcalls into [EuiccChannel] happen on that same thread.
 *  - [stop], [drainLog] and the sink setup calls are safe from any thread.
 */
object NativeBridge {

    init {
        System.loadLibrary("ipacore")
    }

    // --- run loop: src/ipa/run_jni.c -----------------------------------------

    /**
     * Run one IPAd poll cycle to completion using the JSON configuration at
     * [configPath]. Blocks until the eIM has nothing further pending, an error
     * occurs, or [stop] is called.
     *
     * A poll cycle is run-to-completion by design: repeating it is the caller's
     * scheduling decision (WorkManager / AlarmManager on Android — a sleep loop
     * would be killed by Doze).
     *
     * @return 0 on success, or a negative error. [ERR_BUSY] means a run is
     *         already in progress; the native side refuses a second one rather
     *         than sharing the stop flag and nvstate file with the first.
     */
    private external fun nativeRun(configPath: String): Int

    fun run(configPath: String): Int = nativeRun(configPath)

    /**
     * Ask a running [run] to leave its poll loop. Returns immediately; [run]
     * returns once the eIM request and eUICC exchange in flight have finished,
     * so pair this with a join timeout if you need a hard deadline.
     */
    private external fun nativeStop()

    fun stop() = nativeStop()

    // --- log sinks: src/ipa/log_jni.c ----------------------------------------

    /**
     * Install the in-memory ring-buffer log sink, which is what makes
     * [drainLog] return anything. Pass 0 for the default capacity.
     *
     * @return 0 on success, negative on error.
     */
    private external fun nativeLogRingInit(capacityBytes: Int): Int

    fun logRingInit(capacityBytes: Int = 0): Int = nativeLogRingInit(capacityBytes)

    private external fun nativeLogRingFree()

    /** Drop the ring sink; the log reverts to stderr (logcat). */
    fun logRingFree() = nativeLogRingFree()

    /**
     * Log text buffered since the previous call, or null when nothing new has
     * arrived. Destructive: text is returned once, so append it rather than
     * re-rendering.
     */
    private external fun nativeLogDrain(): String?

    fun drainLog(): String? = nativeLogDrain()

    /**
     * Install the rotating-file log sink. [maxSizeBytes] of 0 disables
     * rotation.
     *
     * @return 0 on success, or a negative errno.
     */
    private external fun nativeLogFileInit(path: String, maxSizeBytes: Long, maxFiles: Int): Int

    fun logFileInit(path: String, maxSizeBytes: Long, maxFiles: Int): Int =
        nativeLogFileInit(path, maxSizeBytes, maxFiles)

    private external fun nativeLogFileFree()

    /** Close the log file and revert to the previous sink. */
    fun logFileFree() = nativeLogFileFree()

    /** Native `-EBUSY`: a poll cycle is already running. */
    const val ERR_BUSY = -16
}
