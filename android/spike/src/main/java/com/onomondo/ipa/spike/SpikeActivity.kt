/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

package com.onomondo.ipa.spike

import android.Manifest
import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.text.method.ScrollingMovementMethod
import android.util.TypedValue
import android.view.Gravity
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import com.onomondo.ipa.EuiccChannel
import kotlin.concurrent.thread

/**
 * On-screen driver for the eUICC transport spike (ANDROID_PORT_PLAN.md Phase 1).
 *
 * Framework-Activity (no AppCompat) with a code-built UI so the reusable harness
 * pulls in no theme or support-library requirements from its host app. It reads
 * the host's [DeviceProfile] (or [GenericDeviceProfile]), opens the ISD-R
 * channel through the native core and shows the resulting report.
 */
class SpikeActivity : Activity() {

    private lateinit var slotInput: EditText
    private lateinit var runButton: Button
    private lateinit var copyButton: Button
    private lateinit var statusView: TextView
    private lateinit var reportView: TextView

    private val main = Handler(Looper.getMainLooper())

    private val profile: DeviceProfile
        get() = (application as? DeviceProfileProvider)?.deviceProfile ?: GenericDeviceProfile

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildUi())
        title = "IPA eUICC spike"
        slotInput.setText(profile.defaultSlotIndex.toString())
        statusView.text = "Device profile: ${profile.displayName}\nReady. Insert eUICC and tap Run."
    }

    private fun buildUi(): ScrollView {
        val pad = dp(16)
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }

        col.addView(TextView(this).apply {
            text = "eUICC transport spike"
            setTypeface(typeface, Typeface.BOLD)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
        })

        val slotRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(12), 0, 0)
        }
        slotRow.addView(TextView(this).apply { text = "SIM slot: " })
        slotInput = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_NUMBER
            layoutParams = LinearLayout.LayoutParams(dp(64), WRAP_CONTENT)
        }
        slotRow.addView(slotInput)
        col.addView(slotRow)

        runButton = Button(this).apply {
            text = "Run eUICC spike"
            setOnClickListener { onRun() }
        }
        col.addView(runButton)

        copyButton = Button(this).apply {
            text = "Copy report"
            isEnabled = false
            setOnClickListener { copyReport() }
        }
        col.addView(copyButton)

        statusView = TextView(this).apply { setPadding(0, dp(8), 0, dp(8)) }
        col.addView(statusView)

        reportView = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            setTextIsSelectable(true)
            movementMethod = ScrollingMovementMethod()
            setBackgroundColor(Color.parseColor("#11000000"))
            setPadding(dp(8), dp(8), dp(8), dp(8))
        }
        col.addView(reportView)

        return ScrollView(this).apply {
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT)
            addView(col)
        }
    }

    private fun onRun() {
        val slot = slotInput.text.toString().trim().toIntOrNull() ?: 0
        if (checkSelfPermission(Manifest.permission.READ_PHONE_STATE)
            != PackageManager.PERMISSION_GRANTED
        ) {
            pendingSlot = slot
            requestPermissions(arrayOf(Manifest.permission.READ_PHONE_STATE), REQ_PHONE)
            return
        }
        runSpike(slot)
    }

    private var pendingSlot = 0

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_PHONE) {
            // Proceed regardless: iccOpenLogicalChannel relies on carrier/system
            // privilege, not this runtime grant, and the spike reports failures
            // clearly either way.
            runSpike(pendingSlot)
        }
    }

    private fun runSpike(slot: Int) {
        runButton.isEnabled = false
        copyButton.isEnabled = false
        statusView.text = "Preparing device (${profile.displayName})…"
        reportView.text = ""

        thread(name = "ipa-spike") {
            profile.awaitReady { err ->
                if (err != null) {
                    post { finishRun("Device not ready: $err", "") }
                    return@awaitReady
                }
                post { statusView.text = "Running spike on slot $slot…" }
                val channel = EuiccChannel(applicationContext) { s -> profile.subscriptionIdForSlot(s) }
                val report = try {
                    EuiccSpike.nativeRunSpike(slot)
                } catch (t: Throwable) {
                    "Spike threw: ${t.javaClass.simpleName}: ${t.message}"
                } finally {
                    channel.release()
                }
                post { finishRun("Done.", report) }
            }
        }
    }

    private fun finishRun(status: String, report: String) {
        statusView.text = status
        reportView.text = report
        runButton.isEnabled = true
        copyButton.isEnabled = report.isNotEmpty()
        // Mirror the report to logcat (tag IpaSpikeReport) so it can be captured
        // with `adb logcat -d -s IpaSpikeReport:I` instead of read off-screen.
        report.lineSequence().forEach { android.util.Log.i("IpaSpikeReport", it) }
    }

    private fun copyReport() {
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("ipa-spike-report", reportView.text))
        Toast.makeText(this, "Report copied", Toast.LENGTH_SHORT).show()
    }

    private fun post(block: () -> Unit) = main.post(block)

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    companion object {
        private const val REQ_PHONE = 1
    }
}
