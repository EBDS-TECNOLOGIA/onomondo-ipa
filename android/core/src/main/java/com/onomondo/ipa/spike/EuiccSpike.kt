/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

package com.onomondo.ipa.spike

/**
 * JNI entry point for the native eUICC transport spike (src/ipa/android_spike.c,
 * ANDROID_PORT_PLAN.md Phase 1). [nativeRunSpike] opens the ISD-R logical
 * channel through the *production* native transport (ipa_scard_* → the
 * registered [com.onomondo.ipa.EuiccChannel]) and drives the ES10x 61xx-chaining
 * probes, returning a human-readable report.
 *
 * A [com.onomondo.ipa.EuiccChannel] instance must already be constructed (its
 * init{} registers it with native) before this is called.
 */
object EuiccSpike {

    /**
     * Run the spike against SIM [slot]. Blocks (opens a channel, transceives) --
     * must be called off the main thread. Returns the full report text.
     */
    external fun nativeRunSpike(slot: Int): String

    init {
        // Idempotent with EuiccChannel's own load; guarantees the library is
        // present even if a caller touches EuiccSpike before any EuiccChannel.
        System.loadLibrary("ipacore")
    }
}
