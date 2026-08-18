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
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import com.onomondo.ipa.EuiccChannel
import com.onomondo.ipa.NativeBridge
import com.onomondo.ipa.DeviceProfile
import com.onomondo.ipa.DeviceProfileProvider
import com.onomondo.ipa.GenericDeviceProfile
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
 * A single cycle is run-to-completion — it ends when the eIM has nothing
 * further pending. [ACTION_RUN_ONCE] runs exactly one and stops.
 * [ACTION_START] repeats: run a cycle, wait `poll_interval` from the
 * configuration, run the next, until the user stops it or the interval is 0.
 *
 * The waiting happens here rather than in the core, which stays
 * run-to-completion, and rather than in WorkManager: the operator asked for a
 * loop they start and stop by hand, and its granularity (seconds, on a
 * mains-powered terminal) is below what WorkManager will schedule. A partial
 * wake lock is held for the duration so the interval still elapses with the
 * screen off. For a device on battery that should poll every few hours,
 * AlarmManager's setExactAndAllowWhileIdle is the better instrument — see
 * ANDROID_PORT_PLAN.md, Phase 5.
 */
class IpadService : Service() {

    private var worker: Thread? = null
    private var euiccChannel: EuiccChannel? = null
    private var wakeLock: PowerManager.WakeLock? = null

    /** Guards the wait between cycles; notified to cut a wait short. */
    private val sleepLock = Object()

    @Volatile
    private var stopRequested = false

    private val profile: DeviceProfile
        get() = (application as? DeviceProfileProvider)?.deviceProfile ?: GenericDeviceProfile

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                requestStop()
                return START_NOT_STICKY
            }
            ACTION_RUN_ONCE -> startCycle(repeat = false)
            else -> startCycle(repeat = true)
        }
        // Do not restart automatically after a kill: the poll cycle is not
        // idempotent mid-flight, and the host app decides when to run it.
        return START_NOT_STICKY
    }

    private fun startCycle(repeat: Boolean) {
        if (worker != null) {
            Log.i(TAG, "poll cycle already running")
            return
        }

        startForeground(NOTIFICATION_ID, buildNotification("Starting..."))

        val store = ConfigStore(this)
        store.ensureExists(profile.defaultSlotIndex)

        // The ring sink must exist before the core logs anything, or the first
        // lines go to logcat instead of the UI. Installed once per process and
        // deliberately never freed: the UI drains it asynchronously (every
        // 500 ms), so tearing it down when the service stops would discard
        // whatever the last poll had not picked up yet — which is precisely the
        // tail of the run the operator most wants to read. Re-initialising also
        // wipes the buffer, so it happens once rather than per cycle.
        if (!ringInstalled) {
            NativeBridge.logRingInit()
            ringInstalled = true
        }

        stopRequested = false
        setState(State.RUNNING, "Running")

        worker = thread(name = "ipad-run") {
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
                // the backend calls back into it on this same thread. Held across
                // every cycle rather than re-opened per cycle.
                euiccChannel = EuiccChannel(this, profile::subscriptionIdForSlot)

                if (repeat) acquireWakeLock()
                runCycles(store, repeat)
            } catch (e: Throwable) {
                Log.e(TAG, "poll cycle threw", e)
                setState(State.FAILED, "Error: ${e.javaClass.simpleName}: ${e.message}")
            } finally {
                releaseWakeLock()
                euiccChannel?.release()
                euiccChannel = null
                worker = null
                // Leave a terminal state behind. Stop() sets STOPPING and the
                // loop then just unwinds, so without this the UI sits on
                // "Stopping..." forever even though everything has finished.
                // A failure keeps its own message.
                if (state != State.FAILED) {
                    setState(State.IDLE, if (stopRequested) "Stopped" else "Poll cycle finished")
                }
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
        }
    }

    /**
     * Run cycles until told to stop. The interval is re-read from the
     * configuration between cycles, so editing it in Settings takes effect on
     * the next wait instead of needing a restart.
     *
     * A failed cycle does not end the loop: an unreachable eIM or a busy eUICC
     * is usually temporary, and the operator watching the log is better served
     * by retrying than by a service that quietly exits. The wait between cycles
     * is what keeps a persistent failure from becoming a busy loop.
     */
    private fun runCycles(store: ConfigStore, repeat: Boolean) {
        var cycle = 0

        while (!stopRequested) {
            cycle++
            if (repeat) setState(State.RUNNING, "Running (cycle $cycle)")

            val rc = NativeBridge.run(store.file.absolutePath)

            when {
                rc == 0 -> setState(State.IDLE, "Poll cycle finished")
                rc == NativeBridge.ERR_BUSY -> {
                    // Another run holds the native lock; looping would only
                    // spin on the same refusal.
                    setState(State.FAILED, "Already running")
                    return
                }
                else -> setState(State.FAILED, "Poll cycle failed ($rc)")
            }

            if (!repeat || stopRequested) return

            val seconds = store.pollIntervalSeconds()
            if (seconds <= 0L) {
                // Configured not to repeat: honour the file over the button.
                setState(State.IDLE, "Poll cycle finished (interval is 0)")
                return
            }

            setState(State.SLEEPING, "Next cycle in ${formatDuration(seconds)}")
            waitBetweenCycles(seconds)
        }
    }

    /** Wait, unless a stop arrives first. */
    private fun waitBetweenCycles(seconds: Long) {
        val deadline = SystemClock.elapsedRealtime() + seconds * 1000L

        synchronized(sleepLock) {
            while (!stopRequested) {
                val remaining = deadline - SystemClock.elapsedRealtime()
                if (remaining <= 0L) return
                // Guarded by the loop, so a spurious wakeup just re-checks.
                sleepLock.wait(remaining)
            }
        }
    }

    /**
     * Keep the CPU awake between cycles. Object.wait() counts uptime, not
     * elapsed real time, so without this a device that suspends would stretch a
     * 15-second interval into however long it stayed asleep.
     */
    private fun acquireWakeLock() {
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, WAKE_LOCK_TAG).apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
    }

    private fun requestStop() {
        setState(State.STOPPING, "Stopping...")
        // Two things to interrupt: a native run in flight, which unwinds once
        // the current eIM request and eUICC exchange finish, and a wait between
        // cycles, which ends immediately.
        stopRequested = true
        NativeBridge.stop()
        synchronized(sleepLock) { sleepLock.notifyAll() }
    }

    override fun onDestroy() {
        // A stop from outside (task removed, system shutdown) still has to
        // unwind the native run and cut short any wait, or the thread would
        // outlive the service.
        stopRequested = true
        NativeBridge.stop()
        synchronized(sleepLock) { sleepLock.notifyAll() }
        worker?.join(STOP_TIMEOUT_MS)
        // The ring sink is NOT freed here; see startCycle(). It belongs to the
        // process, not to this service instance.
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

    enum class State { IDLE, RUNNING, SLEEPING, STOPPING, FAILED }

    companion object {
        private const val TAG = "IpadService"
        private const val CHANNEL_ID = "ipad"
        private const val NOTIFICATION_ID = 1
        private const val STOP_TIMEOUT_MS = 10_000L

        private const val WAKE_LOCK_TAG = "ipad:poll"

        /** Process-wide, like the native ring buffer it tracks. */
        @Volatile
        private var ringInstalled = false

        const val ACTION_START = "com.onomondo.ipa.ipad.START"
        const val ACTION_STOP = "com.onomondo.ipa.ipad.STOP"
        const val ACTION_RUN_ONCE = "com.onomondo.ipa.ipad.RUN_ONCE"

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

        /** Run exactly one poll cycle, whatever the configured interval says. */
        fun runOnce(context: Context) {
            val intent = Intent(context, IpadService::class.java).setAction(ACTION_RUN_ONCE)
            context.startForegroundService(intent)
        }

        /** "1 min 30 s", "45 s" -- for the status line, not for parsing. */
        fun formatDuration(seconds: Long): String = when {
            seconds < 60L -> "$seconds s"
            seconds % 60L == 0L -> "${seconds / 60} min"
            else -> "${seconds / 60} min ${seconds % 60} s"
        }

        fun stop(context: Context) {
            val intent = Intent(context, IpadService::class.java).setAction(ACTION_STOP)
            context.startService(intent)
        }
    }
}
