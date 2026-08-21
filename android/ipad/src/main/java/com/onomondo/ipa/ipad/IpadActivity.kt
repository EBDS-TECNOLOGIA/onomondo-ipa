/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

package com.onomondo.ipa.ipad

import android.Manifest
import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import com.onomondo.ipa.NativeBridge
import com.onomondo.ipa.DeviceProfile
import com.onomondo.ipa.DeviceProfileProvider
import com.onomondo.ipa.GenericDeviceProfile

/**
 * Main screen: start/stop the IPAd and watch its log live
 * (ANDROID_PORT_PLAN.md Phase 5).
 *
 * Framework Activity with a code-built UI, matching
 * [com.onomondo.ipa.spike.SpikeActivity]: the module then imposes no theme or
 * support-library requirement on its host app, which matters because the host
 * ships to a locked-down POS terminal. (The plan sketched Compose; the existing
 * modules deliberately avoid even AppCompat, and a live log tail plus four
 * buttons does not justify the dependency.)
 *
 * The log comes from the native ring-buffer sink via [NativeBridge.drainLog],
 * polled on the main looper. Draining is destructive, so lines are appended
 * rather than re-rendered.
 */
class IpadActivity : Activity() {

    private lateinit var statusView: TextView
    private lateinit var logView: TextView
    private lateinit var logScroll: ScrollView
    private lateinit var startButton: Button
    private lateinit var onceButton: Button
    private lateinit var stopButton: Button

    private val main = Handler(Looper.getMainLooper())
    private var polling = false

    private val profile: DeviceProfile
        get() = (application as? DeviceProfileProvider)?.deviceProfile ?: GenericDeviceProfile

    private val poll = object : Runnable {
        override fun run() {
            NativeBridge.drainLog()?.let { appendLog(it) }
            refreshButtons()
            if (polling) main.postDelayed(this, POLL_INTERVAL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildUi())
        title = "IPAd"

        // Seed the config on first launch so Settings has something to show and
        // the service never starts against a missing file.
        ConfigStore(this).ensureExists(profile.defaultSlotIndex)

        requestNotificationPermissionIfNeeded()
    }

    override fun onResume() {
        super.onResume()
        IpadService.onStateChanged = { _, text -> main.post { statusView.text = statusLine(text) } }
        statusView.text = statusLine(IpadService.statusText)
        polling = true
        main.post(poll)
    }

    override fun onPause() {
        super.onPause()
        // Stop polling but leave the service and its ring sink alone: the poll
        // cycle must keep running while the screen is off.
        polling = false
        main.removeCallbacks(poll)
        IpadService.onStateChanged = null
    }

    private fun statusLine(text: String) = "Device: ${profile.displayName}\nStatus: $text"

    private fun appendLog(chunk: String) {
        logView.append(chunk)
        // Keep the view from growing without bound over a long session; the
        // native ring is already capped, this caps what we render.
        val text = logView.text
        if (text.length > MAX_LOG_CHARS) {
            logView.text = text.subSequence(text.length - MAX_LOG_CHARS, text.length)
        }
        logScroll.post { logScroll.fullScroll(ScrollView.FOCUS_DOWN) }
    }

    private fun refreshButtons() {
        // Sleeping between cycles is still "busy": Start would be a no-op and
        // Stop is what ends the loop.
        val busy = IpadService.state == IpadService.State.RUNNING ||
            IpadService.state == IpadService.State.SLEEPING ||
            IpadService.state == IpadService.State.STOPPING
        startButton.isEnabled = !busy
        onceButton.isEnabled = !busy
        stopButton.isEnabled = busy
    }

    private fun requestNotificationPermissionIfNeeded() {
        // A foreground service still runs without it, but its notification is
        // silently dropped on 13+, which makes the app look dead.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) ==
            PackageManager.PERMISSION_GRANTED
        ) return
        requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQ_NOTIFICATIONS)
    }

    private fun buildUi(): LinearLayout {
        val pad = dp(16)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }

        statusView = TextView(this).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            setPadding(0, 0, 0, dp(12))
        }
        root.addView(statusView, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        // Two rows: running the IPAd on the first, everything else on the
        // second. Five equal-width buttons on one row are unreadable on a POS
        // terminal's screen.
        val runRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val toolRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }

        startButton = Button(this).apply {
            text = "Start"
            setOnClickListener {
                IpadService.start(this@IpadActivity)
                refreshButtons()
            }
        }
        onceButton = Button(this).apply {
            text = "Run once"
            setOnClickListener {
                IpadService.runOnce(this@IpadActivity)
                refreshButtons()
            }
        }
        stopButton = Button(this).apply {
            text = "Stop"
            isEnabled = false
            setOnClickListener { IpadService.stop(this@IpadActivity) }
        }
        val settingsButton = Button(this).apply {
            text = "Settings"
            setOnClickListener { startActivity(Intent(this@IpadActivity, SettingsActivity::class.java)) }
        }
        val copyButton = Button(this).apply {
            text = "Copy log"
            setOnClickListener {
                val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("ipad-log", logView.text))
                Toast.makeText(this@IpadActivity, "Log copied", Toast.LENGTH_SHORT).show()
            }
        }
        listOf(startButton, onceButton, stopButton).forEach {
            runRow.addView(it, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        }
        listOf(settingsButton, copyButton).forEach {
            toolRow.addView(it, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        }
        root.addView(runRow, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        root.addView(toolRow, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        logView = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
            setTextColor(Color.DKGRAY)
            setTextIsSelectable(true)
        }
        logScroll = ScrollView(this).apply { addView(logView) }
        root.addView(logScroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        return root
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    companion object {
        private const val POLL_INTERVAL_MS = 500L
        private const val MAX_LOG_CHARS = 200_000
        private const val REQ_NOTIFICATIONS = 1
    }
}
