/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

package com.onomondo.ipa.ipad

import android.content.Context
import org.json.JSONObject
import java.io.File

/**
 * The app's copy of the IPAd JSON configuration (ANDROID_PORT_PLAN.md Phase 5).
 *
 * There is exactly one schema, parsed by `libipa/config_json.c`, and this class
 * does not re-implement it: it reads and writes the same file the native side
 * reads, using org.json so the module needs no dependency beyond the framework.
 * Validation is deliberately left to the native parser, which is strict — an
 * unknown key or a bad value is refused there with a log line naming it, and
 * duplicating those rules in Kotlin would just create two things to keep in
 * step.
 *
 * Paths default into the app's private files directory, which is the only
 * location an unprivileged install can be sure of; a privileged/system install
 * may point them anywhere it likes.
 */
class ConfigStore(context: Context) {

    private val filesDir: File = context.filesDir

    /** The file handed to `NativeBridge.run()`. */
    val file: File = File(filesDir, "config.json")

    val nvstatePath: String get() = File(filesDir, "nvstate.bin").absolutePath
    val logPath: String get() = File(filesDir, "ipa.log").absolutePath

    /**
     * Create the configuration on first run, seeded with [defaultSlotIndex]
     * from the device profile. Existing files are left alone, so an operator's
     * edits survive an app update.
     */
    fun ensureExists(defaultSlotIndex: Int) {
        if (file.exists()) return
        save(defaults(defaultSlotIndex))
    }

    fun defaults(defaultSlotIndex: Int): JSONObject = JSONObject().apply {
        // Keys starting with "_" are ignored by the parser, which is how a
        // JSON file gets to carry a comment.
        put("_comment", "IPAd configuration -- see ANDROID_PORT_PLAN.md for the full key list")
        put("tac", DEFAULT_TAC)
        put("reader_num", defaultSlotIndex)
        put("nvstate_path", nvstatePath)
        put("esipa_req_retries", DEFAULT_RETRIES)
        put("esipa_binding", "asn1")
        put("iot_euicc_emu_enabled", false)
        put("refresh_flag", false)
        // 0 = one poll cycle per Start, which is what the IPAd did before the
        // interval existed. IpadService reads these two keys; the native side
        // accepts them for the ipad(8) daemon's benefit.
        put(KEY_INTERVAL, DEFAULT_INTERVAL)
        put(KEY_INTERVAL_UNIT, UNIT_SECONDS)
        put("log", JSONObject().apply {
            put("path", logPath)
            put("max_size_bytes", DEFAULT_LOG_MAX_SIZE)
            put("max_files", DEFAULT_LOG_MAX_FILES)
        })
        // euicc_channel is deliberately absent: on Android the transport owns
        // the logical channel, so the core ignores it (see scard_manages_channel).
    }

    fun load(): JSONObject =
        if (file.exists()) JSONObject(file.readText()) else defaults(0)

    fun save(config: JSONObject) {
        file.parentFile?.mkdirs()
        file.writeText(config.toString(2))
    }

    fun rawText(): String = if (file.exists()) file.readText() else ""

    /**
     * The configured interval in seconds, or 0 for "one cycle per Start".
     *
     * Mirrors `ipa_run_config_poll_seconds()`: the value is stored in the unit
     * the operator chose so the settings screen can show it back unchanged, and
     * only converted where a duration is actually needed. A file edited by hand
     * into something unparseable falls back to a single cycle rather than
     * throwing — the native parser is what reports bad configuration, and it
     * will refuse the same file a moment later with a precise message.
     */
    fun pollIntervalSeconds(): Long = try {
        val config = load()
        val value = config.optLong(KEY_INTERVAL, DEFAULT_INTERVAL.toLong())
        val unit = config.optString(KEY_INTERVAL_UNIT, UNIT_SECONDS)
        when {
            value <= 0L -> 0L
            unit == UNIT_MINUTES -> value * 60L
            else -> value
        }
    } catch (e: Exception) {
        0L
    }

    companion object {
        const val DEFAULT_TAC = "12345678"
        const val DEFAULT_RETRIES = 3
        const val DEFAULT_LOG_MAX_SIZE = 262144L
        const val DEFAULT_LOG_MAX_FILES = 5

        const val KEY_INTERVAL = "poll_interval"
        const val KEY_INTERVAL_UNIT = "poll_interval_unit"
        const val UNIT_SECONDS = "seconds"
        const val UNIT_MINUTES = "minutes"
        /** What the app seeds a fresh config with: repeat every 15 seconds. */
        const val DEFAULT_INTERVAL = 15

        /** Same floor the native parser enforces; 0 ("do not repeat") is exempt. */
        const val MIN_INTERVAL_SECONDS = 5

        /** Same ceiling the native parser enforces (24 h), in each unit. */
        const val MAX_INTERVAL_SECONDS = 24 * 60 * 60
        const val MAX_INTERVAL_MINUTES = 24 * 60
    }
}
