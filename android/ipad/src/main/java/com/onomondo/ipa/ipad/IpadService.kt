/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

package com.onomondo.ipa.ipad

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import android.util.Log
import com.onomondo.ipa.EuiccChannel
import com.onomondo.ipa.NativeBridge
import com.onomondo.ipa.spike.DeviceProfile
import com.onomondo.ipa.spike.DeviceProfileProvider
import com.onomondo.ipa.spike.GenericDeviceProfile
import java.util.concurrent.CountDownLatch
import kotlin.concurrent.thread

/**
 * Foreground service that runs one IPAd poll cycle (ANDROID_PORT_PLAN.md
 * Phase 5).
 *
 * Why a service rather than a native daemon: reaching the ISD-R over Android
 * telephony requires the calling process to hold MODIFY_PHONE_STATE *and* be
 * the device's LPA, both of which are properties of this package. A process
 * started by init has neither. See contrib/android/README.md.
 *
 * Why foreground: the poll cycle makes network requests and drives the eUICC,
 * so it must survive the Activity going away; a background service would be
 * killed mid-exchange.
 *
 * The cycle is run-to-completion — it ends when the eIM has nothing further
 * pending — so this service stops itself when [NativeBridge.run] returns.
 * Re-running it on a schedule is the host app's job (WorkManager /
 * AlarmManager); a sleep loop here would be killed by Doze.
 */
class IpadService : Service() {

    private var worker: Thread? = null
    private var euiccChannel: EuiccChannel? = null

    private val profile: DeviceProfile
        get() = (application as? DeviceProfileProvider)?.deviceProfile ?: GenericDeviceProfile

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                requestStop()
                return START_NOT_STICKY
            }
            else -> startCycle()
        }
        // Do not restart automatically after a kill: the poll cycle is not
        // idempotent mid-flight, and the host app decides when to run it.
        return START_NOT_STICKY
    }

    private fun startCycle() {
        if (worker != null) {
            Log.i(TAG, "poll cycle already running")
            return
        }

        startForeground(NOTIFICATION_ID, buildNotification("Starting..."))

        val store = ConfigStore(this)
        store.ensureExists(profile.defaultSlotIndex)

        // The ring sink must exist before the core logs anything, or the first
        // lines go to logcat instead of the UI.
        NativeBridge.logRingInit()

        setState(State.RUNNING, "Running")

        worker = thread(name = "ipad-run") {
            var rc: Int
            try {
                // Vendor SDKs (a POS terminal's telephony layer, say) may need
                // booting before the transport works. Contractually called off
                // the main thread, which is where we are.
                var bootError: String? = null
                val ready = CountDownLatch(1)
                profile.awaitReady { error ->
                    bootError = error
                    ready.countDown()
                }
                ready.await()

                if (bootError != null) {
                    Log.e(TAG, "device profile not ready: $bootError")
                    setState(State.FAILED, "Device not ready: $bootError")
                    return@thread
                }

                // Registers itself with the native scard backend on construction;
                // the backend calls back into it on this same thread.
                euiccChannel = EuiccChannel(this, profile::subscriptionIdForSlot)

                rc = NativeBridge.run(store.file.absolutePath)

                when {
                    rc == 0 -> setState(State.IDLE, "Poll cycle finished")
                    rc == NativeBridge.ERR_BUSY -> setState(State.FAILED, "Already running")
                    else -> setState(State.FAILED, "Poll cycle failed ($rc)")
                }
            } catch (e: Throwable) {
                Log.e(TAG, "poll cycle threw", e)
                setState(State.FAILED, "Error: ${e.javaClass.simpleName}: ${e.message}")
            } finally {
                euiccChannel?.release()
                euiccChannel = null
                worker = null
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
        }
    }

    private fun requestStop() {
        setState(State.STOPPING, "Stopping...")
        // Returns immediately; the worker unwinds once the eIM request and
        // eUICC exchange in flight have finished.
        NativeBridge.stop()
    }

    override fun onDestroy() {
        // A stop from outside (task removed, system shutdown) still has to
        // unwind the native run, or the thread would outlive the service.
        NativeBridge.stop()
        worker?.join(STOP_TIMEOUT_MS)
        NativeBridge.logRingFree()
        super.onDestroy()
    }

    private fun buildNotification(text: String): Notification {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        // IMPORTANCE_LOW: the daemon runs unattended, so the notification is
        // there to keep the process alive and to be tappable, not to interrupt.
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "IPAd", NotificationManager.IMPORTANCE_LOW)
        )

        // A launcher intent is normally present, but a headless install (no
        // launcher activity) is legitimate -- fall back to a notification with
        // no tap target rather than throwing.
        val launch = packageManager.getLaunchIntentForPackage(packageName)
        val open = launch?.let {
            PendingIntent.getActivity(
                this, 0, it,
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            )
        }

        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("IPAd")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setOngoing(true)
            .apply { open?.let { setContentIntent(it) } }
            .build()
    }

    enum class State { IDLE, RUNNING, STOPPING, FAILED }

    companion object {
        private const val TAG = "IpadService"
        private const val CHANNEL_ID = "ipad"
        private const val NOTIFICATION_ID = 1
        private const val STOP_TIMEOUT_MS = 10_000L

        const val ACTION_START = "com.onomondo.ipa.ipad.START"
        const val ACTION_STOP = "com.onomondo.ipa.ipad.STOP"

        /**
         * Process-local state for the UI. Deliberately not a bound service or a
         * broadcast: the Activity and the service always share a process here,
         * and this keeps the surface to what the UI actually needs.
         */
        @Volatile
        var state: State = State.IDLE
            private set

        @Volatile
        var statusText: String = "Idle"
            private set

        /**
         * Set by the Activity while visible. Invoked on whichever thread
         * changed the state -- usually the worker -- so a listener that touches
         * views must post to the main looper itself.
         */
        @Volatile
        var onStateChanged: ((State, String) -> Unit)? = null

        private fun setState(newState: State, text: String) {
            state = newState
            statusText = text
            onStateChanged?.invoke(newState, text)
        }

        fun start(context: Context) {
            val intent = Intent(context, IpadService::class.java).setAction(ACTION_START)
            context.startForegroundService(intent)
        }

        fun stop(context: Context) {
            val intent = Intent(context, IpadService::class.java).setAction(ACTION_STOP)
            context.startService(intent)
        }
    }
}
