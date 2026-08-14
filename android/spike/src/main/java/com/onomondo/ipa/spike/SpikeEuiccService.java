/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

package com.onomondo.ipa.spike;

import android.service.euicc.EuiccService;
import android.service.euicc.GetDefaultDownloadableSubscriptionListResult;
import android.service.euicc.GetDownloadableSubscriptionMetadataResult;
import android.service.euicc.GetEuiccProfileInfoListResult;
import android.telephony.euicc.DownloadableSubscription;
import android.telephony.euicc.EuiccInfo;

/**
 * Minimal LPA registration so this app can open a logical channel to the ISD-R.
 *
 * Android's PhoneInterfaceManager.iccOpenLogicalChannel special-cases the ISD-R
 * AID: it only permits the package that EuiccConnector.findBestComponent()
 * selects as the LPA (the best {@code EuiccService}). That is a pure package
 * identity check -- it does not bind or invoke the service -- so to pass it we
 * only need to *exist* as the winning EuiccService (system app + a service
 * guarded by BIND_EUICC_SERVICE + a high intent-filter priority; declared in the
 * manifest). See ANDROID_PORT_PLAN.md, Phase 1.
 *
 * Because the framework never calls these callbacks for the spike, every one is
 * a no-op returning a "nothing here" value. This deliberately makes the device's
 * real eSIM UI non-functional while installed -- acceptable on a test device,
 * and the reason this is confined to the spike harness.
 *
 * EuiccService is a @SystemApi absent from the public SDK, so this file is
 * compiled against spike/libs/euicc-system-stubs.jar (compileOnly; the real
 * classes come from the framework at runtime and are never packaged).
 */
public final class SpikeEuiccService extends EuiccService {

    @Override
    public String onGetEid(int slotId) {
        return null;
    }

    @Override
    public int onGetOtaStatus(int slotId) {
        return 0; // EUICC_OTA_STATUS_UNAVAILABLE
    }

    @Override
    public void onStartOtaIfNecessary(int slotId, OtaStatusChangedCallback statusChangedCallback) {
        // no-op
    }

    @Override
    public GetDownloadableSubscriptionMetadataResult onGetDownloadableSubscriptionMetadata(
            int slotId, DownloadableSubscription subscription, boolean forceDeactivateSim) {
        return null;
    }

    @Override
    public GetDefaultDownloadableSubscriptionListResult onGetDefaultDownloadableSubscriptionList(
            int slotId, boolean forceDeactivateSim) {
        return null;
    }

    @Override
    public GetEuiccProfileInfoListResult onGetEuiccProfileInfoList(int slotId) {
        return null;
    }

    @Override
    public EuiccInfo onGetEuiccInfo(int slotId) {
        return null;
    }

    @Override
    public int onDeleteSubscription(int slotId, String iccid) {
        return RESULT_FIRST_USER;
    }

    @Override
    public int onSwitchToSubscription(int slotId, String iccid, boolean forceDeactivateSim) {
        return RESULT_FIRST_USER;
    }

    @Override
    public int onUpdateSubscriptionNickname(int slotId, String iccid, String nickname) {
        return RESULT_FIRST_USER;
    }

    @Override
    public int onEraseSubscriptions(int slotId) {
        return RESULT_FIRST_USER;
    }

    @Override
    public int onRetainSubscriptionsForFactoryReset(int slotId) {
        return RESULT_FIRST_USER;
    }
}
