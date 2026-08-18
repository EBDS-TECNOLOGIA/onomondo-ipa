/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

package com.onomondo.ipa.device.tectoy

import android.app.Application
import android.util.Log
import br.com.tectoy.dal.HardwareServiceListenerSP
import br.com.tectoy.dal.SPIDal
import br.com.tectoylib.tectoysallmodules.TectoyUser
import com.onomondo.ipa.DeviceProfile
import com.onomondo.ipa.DeviceProfileProvider

/**
 * Application for the Tectoy POS build. Its only jobs are to boot the Tectoy
 * hardware SDK (asynchronous, exactly as the vendor demo does) and to expose a
 * [DeviceProfile] to the reusable spike harness. Nothing device-specific leaks
 * outside this module.
 */
class TectoyApplication : Application(), DeviceProfileProvider {

    /** Set once the vendor service is connected; read by [TectoyDeviceProfile]. */
    @Volatile
    var spiDal: SPIDal? = null
        private set

    override val deviceProfile: DeviceProfile by lazy { TectoyDeviceProfile(this) }

    override fun onCreate() {
        super.onCreate()
        bootTectoySdk()
    }

    private fun bootTectoySdk() {
        try {
            TectoyUser.getInstance().getSPDal(this, object : HardwareServiceListenerSP {
                override fun onServiceConnected(dal: SPIDal) {
                    spiDal = dal
                    Log.i(TAG, "Tectoy SDK connected")
                }

                override fun onServiceDisconnected() {
                    spiDal = null
                    Log.w(TAG, "Tectoy SDK disconnected")
                }
            })
        } catch (t: Throwable) {
            // Not fatal to the spike: without the SDK the harness falls back to
            // AOSP SubscriptionManager for slot→subId resolution.
            Log.e(TAG, "Tectoy SDK boot failed", t)
        }
    }

    companion object {
        private const val TAG = "IpaTectoyApp"
    }
}
