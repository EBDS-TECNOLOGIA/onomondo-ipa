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

    companion object {
        const val DEFAULT_TAC = "12345678"
        const val DEFAULT_RETRIES = 3
        const val DEFAULT_LOG_MAX_SIZE = 262144L
        const val DEFAULT_LOG_MAX_FILES = 5
    }
}
