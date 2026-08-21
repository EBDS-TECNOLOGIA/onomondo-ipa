/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

package com.onomondo.ipa

/**
 * The seam that keeps device-specific setup out of the reusable core library.
 *
 * The IPA will run on many devices; each can differ in which SIM slot holds the
 * eUICC, how a slot maps to a subscription id, and what vendor SDK must be
 * booted first. A device integration implements [DeviceProfile] (in its own
 * module, with its own proprietary dependencies) and exposes it via
 * [DeviceProfileProvider] on its Application. The consumers -- the IPAd
 * service/UI and the spike diagnostic -- ask the Application for a profile and
 * fall back to [GenericDeviceProfile] (stock AOSP) when none is provided, so
 * this module never references any vendor library.
 */
interface DeviceProfile {

    /** Human-readable name shown in the report header. */
    val displayName: String

    /** SIM slot index that holds the eUICC by default. */
    val defaultSlotIndex: Int

    /**
     * Optional device-specific slot→subscription-id resolver, passed straight
     * to [com.onomondo.ipa.EuiccChannel]. Return null to use AOSP's
     * SubscriptionManager. Wire a vendor SDK's own lookup here when it is more
     * reliable than the platform default.
     */
    fun subscriptionIdForSlot(slotIndex: Int): Int? = null

    /**
     * Block until any vendor SDK the transport depends on is ready, then invoke
     * [onReady] with null on success or an error message on failure. The default
     * is immediate success (nothing to boot). Always called off the main thread.
     */
    fun awaitReady(onReady: (error: String?) -> Unit) = onReady(null)
}

/** Stock-AOSP profile: no vendor SDK, slot 0, platform SubscriptionManager. */
object GenericDeviceProfile : DeviceProfile {
    override val displayName = "Generic (AOSP TelephonyManager)"
    override val defaultSlotIndex = 0
}

/**
 * Implemented by an Application to hand its [DeviceProfile] to this module.
 * Keeping this on the Application (rather than a static registry) is what lets a
 * device module own its setup without this module importing it.
 */
interface DeviceProfileProvider {
    val deviceProfile: DeviceProfile
}
