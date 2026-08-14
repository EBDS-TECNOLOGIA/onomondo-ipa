/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

package com.onomondo.ipa.device.tectoy

import android.util.Log
import com.onomondo.ipa.spike.DeviceProfile

/**
 * [DeviceProfile] for the Tectoy POS terminal.
 *
 * Contributes two device-specific things to the otherwise generic spike:
 *  - [awaitReady] blocks until the async Tectoy SDK connection is up, so the
 *    slot→subId lookup below is usable when the spike runs.
 *  - [subscriptionIdForSlot] uses the vendor SPIPhoneManager.getSubIdSP() rather
 *    than AOSP's SubscriptionManager, which is more reliable on this hardware.
 *    Any failure returns null so the transport transparently falls back to AOSP.
 */
class TectoyDeviceProfile(private val app: TectoyApplication) : DeviceProfile {

    override val displayName = "Tectoy POS terminal"

    override val defaultSlotIndex = 0

    override fun subscriptionIdForSlot(slotIndex: Int): Int? {
        return try {
            val subs = app.spiDal?.getSPIPhoneManager()?.getSubIdSP(slotIndex)
            subs?.firstOrNull { it >= 0 }?.also {
                Log.i(TAG, "Tectoy getSubIdSP(slot=$slotIndex) -> $it")
            }
        } catch (t: Throwable) {
            Log.w(TAG, "Tectoy getSubIdSP(slot=$slotIndex) failed; falling back to AOSP", t)
            null
        }
    }

    override fun awaitReady(onReady: (error: String?) -> Unit) {
        val deadline = System.currentTimeMillis() + SDK_TIMEOUT_MS
        while (app.spiDal == null && System.currentTimeMillis() < deadline) {
            try {
                Thread.sleep(100)
            } catch (ie: InterruptedException) {
                Thread.currentThread().interrupt()
                break
            }
        }
        if (app.spiDal == null) {
            // Non-fatal: proceed on AOSP telephony. The spike will still open the
            // channel via SubscriptionManager; getSubIdSP simply won't be used.
            Log.w(TAG, "Tectoy SDK not connected within ${SDK_TIMEOUT_MS}ms; using AOSP path")
        }
        onReady(null)
    }

    companion object {
        private const val TAG = "IpaTectoyProfile"
        private const val SDK_TIMEOUT_MS = 5000L
    }
}
