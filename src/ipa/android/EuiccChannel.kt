/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

package com.onomondo.ipa

import android.content.Context
import android.telephony.IccOpenLogicalChannelResponse
import android.telephony.SubscriptionManager
import android.telephony.TelephonyManager
import android.util.Log
import java.io.IOException

/**
 * eUICC transport for the native IPA core (libipacore.so), Phase 1 of
 * ANDROID_PORT_PLAN.md.
 *
 * The native scard backend (scard_android.c) calls [openChannel], [transmit]
 * and [closeChannel] over JNI; this class fulfils them with the Android
 * telephony logical-channel APIs. As a system/signature app the
 * MODIFY_PHONE_STATE grant these APIs require is satisfied by the platform
 * signature (declared in the manifest, Phase 5).
 *
 * The framework owns the channel: iccOpenLogicalChannel performs MANAGE CHANNEL
 * + the ISD-R SELECT, and iccTransmitApduLogicalChannel sets the CLA channel
 * bits itself — which is why the native core reports scard_manages_channel and
 * does none of that.
 *
 * Construct one instance (per active SIM/eUICC), then start the native core;
 * call [release] on teardown.
 *
 * [slotToSubId] is an optional device-specific hook to map a SIM slot index to
 * a subscription id. It exists so a vendor integration can use a more reliable
 * platform API than AOSP's SubscriptionManager (e.g. a POS SDK's own
 * slot→subId lookup) without this class depending on any vendor library. When
 * it is null, or returns null for a slot, the standard SubscriptionManager path
 * is used. See android/ (the spike app) DeviceProfile.
 */
class EuiccChannel(
    context: Context,
    private val slotToSubId: ((Int) -> Int?)? = null,
) {

    private val appContext = context.applicationContext
    private val baseTm =
        appContext.getSystemService(Context.TELEPHONY_SERVICE) as TelephonyManager

    /** TelephonyManager bound to the subscription of the slot we opened. */
    private var tm: TelephonyManager? = null

    init {
        nativeRegister()
    }

    // --- JNI: implemented in scard_jni.c -------------------------------------
    private external fun nativeRegister()
    private external fun nativeUnregister()

    // --- Called from native (scard_android.c) over JNI ------------------------

    /**
     * Open a logical channel to [aid] (the ISD-R) on SIM slot [slotIndex].
     * @return the framework-assigned channel number (>= 0), or -1 on failure
     *         (native treats any negative value as an error).
     */
    fun openChannel(slotIndex: Int, aid: ByteArray): Int {
        val tmForSlot = telephonyManagerForSlot(slotIndex)
        if (tmForSlot == null) {
            Log.e(TAG, "openChannel: no active subscription for slot $slotIndex")
            return -1
        }
        return try {
            // TERMINAL CAPABILITY (basic channel), mirroring the Linux core's
            // send_termcap. Some eUICCs (observed on a Thales "GTO" eUICC) reject
            // device-side ES10 with SW=6985 unless the terminal has advertised
            // eUICC/LPA support this way; the modem's power-on TERMINAL CAPABILITY
            // may not set those bits. 80 AA 00 00 05 A9 03 84 01 01.
            try {
                val tc = tmForSlot.iccTransmitApduBasicChannel(0x80, 0xAA, 0x00, 0x00, 0x05, "A903840101")
                Log.i(TAG, "TERMINAL CAPABILITY -> ${tc ?: "null"}")
            } catch (e: Exception) {
                Log.w(TAG, "TERMINAL CAPABILITY send failed (basic channel): ${e.javaClass.simpleName}: ${e.message}")
            }

            val resp: IccOpenLogicalChannelResponse =
                tmForSlot.iccOpenLogicalChannel(aid.toHex())
            if (resp.status != IccOpenLogicalChannelResponse.STATUS_NO_ERROR) {
                Log.e(TAG, "openChannel: status=${resp.status}")
                return -1
            }
            tm = tmForSlot
            // Log the SELECT (FCI) response the modem got when it selected the
            // AID: a valid ISD-R FCI proves the channel is in the ISD-R context
            // (so any later 6985 is the eUICC rejecting ES10, not a misrouted
            // channel); an empty/error FCI means the AID was not really selected.
            val fci = resp.selectResponse
            Log.i(TAG, "openChannel: channel=${resp.channel} status=${resp.status} " +
                    "selectResponse=${fci?.toHex() ?: "null"}")
            resp.channel
        } catch (e: SecurityException) {
            // Missing MODIFY_PHONE_STATE / not a privileged app.
            Log.e(TAG, "openChannel: not permitted", e)
            -1
        }
    }

    /**
     * Transmit one raw APDU on [channel] and return the raw response including
     * the trailing SW1SW2. Throws on transport error (native maps that to -EIO).
     *
     * The native core hands us a complete APDU; we decompose it into the
     * CLA/INS/P1/P2/P3/data shape iccTransmitApduLogicalChannel expects and
     * clear the CLA logical-channel bits (the framework re-adds the real ones).
     *
     * IMPORTANT (ANDROID_PORT_PLAN.md, highest-risk item): the core drives its
     * own 61xx GET RESPONSE loop (euicc.c). This method therefore passes the
     * framework's response through *verbatim* and must NOT itself follow 61xx
     * chaining. If a given modem/RIL auto-handles 61xx and returns the fully
     * assembled body with SW=9000 on the triggering command, the core's loop
     * would never see 61xx and would lose data — this is the behaviour to
     * validate on real hardware before relying on this backend. Do not "fix" it
     * by re-chaining here without first re-checking euicc.c's expectations.
     */
    fun transmit(channel: Int, apdu: ByteArray): ByteArray {
        val t = tm ?: throw IllegalStateException("transmit before openChannel")
        if (apdu.size < 4) throw IllegalArgumentException("APDU too short (${apdu.size} bytes)")

        // Set the CLA logical-channel bits to [channel]. iccTransmitApduLogicalChannel
        // routes by the channel argument, but RILs differ on whether they also
        // rewrite the CLA channel bits: some pass CLA through verbatim, so
        // *clearing* the bits sends the APDU on the basic channel (hitting the
        // USIM, not the ISD-R selected on this channel → SW=6985). Setting the
        // bits to [channel] ourselves is correct whether or not the RIL re-adds
        // them. Channels 0-3 use the interindustry low-nibble encoding; 4-19 use
        // the extended encoding (bit 0x40 set, channel-4 in the low bits).
        val claByte = apdu[0].toInt() and 0xFF
        val cla = when (channel) {
            in 0..3 -> (claByte and 0xFC) or channel
            in 4..19 -> (claByte and 0xB0) or 0x40 or (channel - 4)
            else -> claByte
        }
        val ins = apdu[1].toInt() and 0xFF
        val p1 = apdu[2].toInt() and 0xFF
        val p2 = apdu[3].toInt() and 0xFF

        val p3: Int
        val dataHex: String
        when {
            apdu.size == 4 -> { p3 = 0; dataHex = "" }               // case 1: no Lc/Le
            apdu.size == 5 -> { p3 = apdu[4].toInt() and 0xFF; dataHex = "" } // case 2: Le only
            else -> {                                                // case 4: Lc + data
                val lc = apdu[4].toInt() and 0xFF
                val end = minOf(5 + lc, apdu.size)
                p3 = lc
                dataHex = apdu.copyOfRange(5, end).toHex()
            }
        }

        val respHex = t.iccTransmitApduLogicalChannel(channel, cla, ins, p1, p2, p3, dataHex)
            ?: throw IOException("iccTransmitApduLogicalChannel returned null")
        return respHex.hexToBytes()
    }

    /** Close [channel]. @return true on success. */
    fun closeChannel(channel: Int): Boolean {
        return try {
            val ok = tm?.iccCloseLogicalChannel(channel) ?: false
            Log.i(TAG, "closeChannel($channel) -> $ok")
            ok
        } catch (e: SecurityException) {
            Log.e(TAG, "closeChannel: not permitted", e)
            false
        }
    }

    /** Detach from native; call on teardown. */
    fun release() {
        nativeUnregister()
        tm = null
    }

    // --- helpers -------------------------------------------------------------

    /** Bind a TelephonyManager to the subscription currently in [slotIndex]. */
    private fun telephonyManagerForSlot(slotIndex: Int): TelephonyManager? {
        // Prefer a device-specific slot→subId resolver when one is supplied and
        // yields a usable id; fall back to AOSP's SubscriptionManager otherwise.
        slotToSubId?.invoke(slotIndex)?.let { subId ->
            if (subId >= 0) {
                Log.i(TAG, "slot $slotIndex → subId $subId (device resolver)")
                return baseTm.createForSubscriptionId(subId)
            }
        }
        val sm = appContext.getSystemService(Context.TELEPHONY_SUBSCRIPTION_SERVICE)
                as? SubscriptionManager ?: return null
        val info = try {
            sm.getActiveSubscriptionInfoForSimSlotIndex(slotIndex)
        } catch (e: SecurityException) {
            Log.e(TAG, "no permission to read subscription for slot $slotIndex", e)
            null
        } ?: return null
        return baseTm.createForSubscriptionId(info.subscriptionId)
    }

    private fun ByteArray.toHex(): String {
        val sb = StringBuilder(size * 2)
        for (b in this) sb.append(HEX[(b.toInt() ushr 4) and 0xF]).append(HEX[b.toInt() and 0xF])
        return sb.toString()
    }

    private fun String.hexToBytes(): ByteArray {
        require(length % 2 == 0) { "odd-length hex response" }
        val out = ByteArray(length / 2)
        var i = 0
        while (i < length) {
            out[i / 2] = ((hexNibble(this[i]) shl 4) or hexNibble(this[i + 1])).toByte()
            i += 2
        }
        return out
    }

    private fun hexNibble(c: Char): Int = when (c) {
        in '0'..'9' -> c - '0'
        in 'a'..'f' -> c - 'a' + 10
        in 'A'..'F' -> c - 'A' + 10
        else -> throw IOException("invalid hex digit '$c'")
    }

    companion object {
        private const val TAG = "IpaEuiccChannel"
        private val HEX = "0123456789ABCDEF".toCharArray()

        init {
            System.loadLibrary("ipacore")
        }
    }
}
