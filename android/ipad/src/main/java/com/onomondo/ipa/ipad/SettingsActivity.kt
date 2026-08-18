/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

package com.onomondo.ipa.ipad

import android.app.Activity
import android.graphics.Typeface
import android.os.Bundle
import android.text.InputType
import android.util.TypedValue
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.ToggleButton
import android.widget.Toast
import org.json.JSONObject

/**
 * Settings screen: edits the same `config.json` the native core parses
 * (ANDROID_PORT_PLAN.md Phase 5).
 *
 * It edits the file rather than shadowing it in SharedPreferences, so there is
 * one source of truth and an operator can equally well push a config with `adb`
 * and see it here. Fields left blank are removed from the JSON, which makes the
 * native parser fall back to its documented default — that is why "unset" and
 * "empty string" are not the same thing here.
 *
 * Only the keys worth touching on a device are exposed; the full schema is
 * larger. Anything not listed survives a save untouched, because the existing
 * JSONObject is edited rather than rebuilt.
 *
 * One exception to leaving validation native: the poll interval is checked here
 * too, so a bad value is refused while it is being typed rather than at the
 * start of the next run. See [saveInterval].
 */
class SettingsActivity : Activity() {

    private lateinit var store: ConfigStore
    private lateinit var config: JSONObject

    private lateinit var tac: EditText
    private lateinit var readerNum: EditText
    private lateinit var preferredEim: EditText
    private lateinit var retries: EditText
    private lateinit var caBundle: EditText
    private lateinit var logMaxSize: EditText
    private lateinit var logMaxFiles: EditText
    private lateinit var disableSsl: CheckBox
    private lateinit var disableSslVerif: CheckBox
    private lateinit var iotEmulation: CheckBox
    private lateinit var refreshFlag: CheckBox
    private lateinit var jsonBinding: CheckBox
    private lateinit var pollInterval: EditText
    private lateinit var pollUnit: ToggleButton

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        title = "IPAd settings"

        store = ConfigStore(this)
        store.ensureExists(0)
        config = store.load()

        setContentView(buildUi())
        populate()
    }

    private fun populate() {
        tac.setText(config.optString("tac", ConfigStore.DEFAULT_TAC))
        readerNum.setText(config.optInt("reader_num", 0).toString())
        preferredEim.setText(config.optString("preferred_eim_id", ""))
        retries.setText(config.optInt("esipa_req_retries", ConfigStore.DEFAULT_RETRIES).toString())
        caBundle.setText(config.optString("eim_cabundle", ""))

        disableSsl.isChecked = config.optBoolean("eim_disable_ssl", false)
        disableSslVerif.isChecked = config.optBoolean("eim_disable_ssl_verif", false)
        iotEmulation.isChecked = config.optBoolean("iot_euicc_emu_enabled", false)
        refreshFlag.isChecked = config.optBoolean("refresh_flag", false)
        jsonBinding.isChecked = config.optString("esipa_binding", "asn1") == "json"

        pollInterval.setText(
            config.optInt(ConfigStore.KEY_INTERVAL, ConfigStore.DEFAULT_INTERVAL).toString()
        )
        pollUnit.isChecked =
            config.optString(ConfigStore.KEY_INTERVAL_UNIT, ConfigStore.UNIT_SECONDS) ==
                ConfigStore.UNIT_MINUTES

        val log = config.optJSONObject("log") ?: JSONObject()
        logMaxSize.setText(log.optLong("max_size_bytes", ConfigStore.DEFAULT_LOG_MAX_SIZE).toString())
        logMaxFiles.setText(log.optInt("max_files", ConfigStore.DEFAULT_LOG_MAX_FILES).toString())
    }

    /**
     * Write the fields back. Validation is left to the native parser, which is
     * strict and reports the offending key; re-implementing its rules here
     * would create a second thing to keep in step. What this does guarantee is
     * well-formed JSON of the right *types*.
     */
    private fun save() {
        if (!saveInterval()) return

        config.put("tac", tac.text.toString().trim())
        config.put("reader_num", readerNum.text.toString().trim().toIntOrNull() ?: 0)
        config.put("esipa_req_retries", retries.text.toString().trim().toIntOrNull()
            ?: ConfigStore.DEFAULT_RETRIES)

        putOrRemove("preferred_eim_id", preferredEim.text.toString().trim())
        putOrRemove("eim_cabundle", caBundle.text.toString().trim())

        config.put("eim_disable_ssl", disableSsl.isChecked)
        config.put("eim_disable_ssl_verif", disableSslVerif.isChecked)
        config.put("iot_euicc_emu_enabled", iotEmulation.isChecked)
        config.put("refresh_flag", refreshFlag.isChecked)
        config.put("esipa_binding", if (jsonBinding.isChecked) "json" else "asn1")

        val log = config.optJSONObject("log") ?: JSONObject().also { config.put("log", it) }
        log.put("path", log.optString("path", store.logPath))
        log.put("max_size_bytes", logMaxSize.text.toString().trim().toLongOrNull()
            ?: ConfigStore.DEFAULT_LOG_MAX_SIZE)
        log.put("max_files", logMaxFiles.text.toString().trim().toIntOrNull()
            ?: ConfigStore.DEFAULT_LOG_MAX_FILES)

        store.save(config)
        Toast.makeText(this, "Saved to ${store.file.name}", Toast.LENGTH_SHORT).show()
        finish()
    }

    /**
     * The one field validated here rather than left to the native parser.
     * The floor exists because a cycle that talks to the eIM over the network
     * takes longer than a couple of seconds anyway, and refusing it at the
     * moment it is typed beats a run that fails minutes later with the reason
     * buried in the log. The same rule is enforced in `config_json.c`, which
     * remains the authority for a file edited by hand.
     *
     * @return true when the interval was accepted and written.
     */
    private fun saveInterval(): Boolean {
        val minutes = pollUnit.isChecked
        val value = pollInterval.text.toString().trim().toIntOrNull()

        if (value == null || value < 0) {
            Toast.makeText(this, "Poll interval must be a whole number", Toast.LENGTH_LONG).show()
            return false
        }

        val seconds = if (minutes) value * 60 else value
        if (value > 0 && seconds < ConfigStore.MIN_INTERVAL_SECONDS) {
            Toast.makeText(
                this,
                "Poll interval must be 0 (single cycle) or at least " +
                    "${ConfigStore.MIN_INTERVAL_SECONDS} seconds",
                Toast.LENGTH_LONG
            ).show()
            return false
        }

        val max = if (minutes) ConfigStore.MAX_INTERVAL_MINUTES else ConfigStore.MAX_INTERVAL_SECONDS
        if (value > max) {
            Toast.makeText(
                this,
                "Poll interval must be at most $max ${if (minutes) "minutes" else "seconds"}",
                Toast.LENGTH_LONG
            ).show()
            return false
        }

        config.put(ConfigStore.KEY_INTERVAL, value)
        config.put(
            ConfigStore.KEY_INTERVAL_UNIT,
            if (minutes) ConfigStore.UNIT_MINUTES else ConfigStore.UNIT_SECONDS
        )
        return true
    }

    /** An empty field means "unset", which is not the same as an empty value. */
    private fun putOrRemove(key: String, value: String) {
        if (value.isEmpty()) config.remove(key) else config.put(key, value)
    }

    private fun buildUi(): ScrollView {
        val pad = dp(16)
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }

        tac = col.field("TAC (8 hex digits)", InputType.TYPE_CLASS_TEXT)
        readerNum = col.field("SIM slot (reader_num)", InputType.TYPE_CLASS_NUMBER)
        preferredEim = col.field("Preferred eIM id (blank = first configured)", InputType.TYPE_CLASS_TEXT)
        retries = col.field("ESipa request retries", InputType.TYPE_CLASS_NUMBER)
        caBundle = col.field("CA bundle path (blank = system trust store)", InputType.TYPE_CLASS_TEXT)

        disableSsl = col.check("Disable HTTPS (test only)")
        disableSslVerif = col.check("Disable certificate verification (insecure)")
        iotEmulation = col.check("Emulate IoT eUICC (consumer eUICC compatibility)")
        refreshFlag = col.check("Request UICC REFRESH on profile change")
        jsonBinding = col.check("Use JSON ESipa binding (default: ASN.1)")

        col.heading("Polling")
        pollInterval = col.field(
            "Interval between cycles (0 = single cycle per Start)",
            InputType.TYPE_CLASS_NUMBER
        )
        pollUnit = ToggleButton(this).apply {
            textOff = "seconds"
            textOn = "minutes"
            // ToggleButton shows textOff/textOn only after isChecked is applied.
            isChecked = false
        }
        col.addView(pollUnit, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))

        col.heading("Log file")
        logMaxSize = col.field("Rotate at (bytes; 0 = never)", InputType.TYPE_CLASS_NUMBER)
        logMaxFiles = col.field("Files to keep (current included)", InputType.TYPE_CLASS_NUMBER)

        col.addView(Button(this).apply {
            text = "Save"
            setOnClickListener { save() }
        }, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        col.addView(TextView(this).apply {
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f)
            text = "\nFile: ${store.file.absolutePath}\n" +
                "Keys not shown here are preserved; edit the file directly for the full schema."
        }, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        return ScrollView(this).apply { addView(col) }
    }

    private fun LinearLayout.heading(text: String) {
        addView(TextView(this@SettingsActivity).apply {
            this.text = text
            typeface = Typeface.DEFAULT_BOLD
            setPadding(0, dp(16), 0, dp(4))
        }, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
    }

    private fun LinearLayout.field(label: String, inputType: Int): EditText {
        addView(TextView(this@SettingsActivity).apply {
            text = label
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            setPadding(0, dp(8), 0, 0)
        }, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        val edit = EditText(this@SettingsActivity).apply {
            this.inputType = inputType
            setSingleLine()
        }
        addView(edit, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        return edit
    }

    private fun LinearLayout.check(label: String): CheckBox {
        val box = CheckBox(this@SettingsActivity).apply { text = label }
        addView(box, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        return box
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()
}
