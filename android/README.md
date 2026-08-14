# IPA Android app

Two things live here: the **IPAd application** (Phase 5 of
`ANDROID_PORT_PLAN.md`) and the **Phase-1 eUICC spike** it grew out of. Both
drive the same native core (`libipacore.so`) through the same production
transport, and both ship in the same APK — the IPAd is the launcher, the spike
stays installed as a diagnostic.

## The IPAd app

`IpadActivity` starts and stops a foreground service (`IpadService`) that runs
one native poll cycle via `NativeBridge.run(configPath)`, and tails the core's
log live from the in-memory ring sink. `SettingsActivity` edits the very same
`config.json` the native parser reads — there is one schema and one file, not a
Kotlin shadow copy of it.

A poll cycle is run-to-completion: it ends when the eIM has nothing further
pending, and the service then stops itself. Re-running it on a schedule is the
host app's job (`WorkManager`/`AlarmManager`); a sleep loop would be killed by
Doze.

Reach the diagnostic with:

    adb shell am start -n <applicationId>/com.onomondo.ipa.spike.SpikeActivity

## The Phase-1 spike

A small wrapper that drives the native core on real hardware to answer the
**Phase 1 gating question** of `ANDROID_PORT_PLAN.md`:
does the Android telephony logical-channel transport expose ISO **61xx GET
RESPONSE chaining** to us (so the core's own loop is needed and works), or does
the modem/RIL auto-assemble long responses? It also confirms the ISD-R channel
can be opened at all on the target device.

The app opens the ISD-R channel through the *production* transport
(`ipa_scard_*` → `EuiccChannel` over JNI, i.e. the exact code Phase 1 ships),
sends a few well-known ES10x commands, records every raw APDU exchange, and
prints a verdict on screen.

## Layout — device setup is kept separate from the harness

    :spike          Android library. Device-agnostic and reusable: the eUICC
                    transport (EuiccChannel) and the JNI face of the core
                    (NativeBridge), both shared verbatim from
                    ../src/ipa/android; the native spike JNI (EuiccSpike); the
                    spike UI (SpikeActivity); the DeviceProfile seam; and the
                    LPA (EuiccService) declaration plus telephony permissions
                    that ISD-R access depends on. Ships libipacore.so in its
                    jniLibs. No vendor deps.

    :ipad           Android library. The IPAd application layer: IpadService
                    (foreground poll cycle), IpadActivity (start/stop + live
                    log), SettingsActivity and ConfigStore (config.json).
                    Device-agnostic; builds on :spike. Framework-only UI, so it
                    imposes no theme or support library on its host.

    :app-generic    Application. Runs on stock AOSP telephony with NO
                    proprietary dependencies. Build/install this on any device
                    (including the Tectoy one, via AOSP) as a baseline.

    :device-tectoy  Application. Device-specific build for the Tectoy POS
                    terminal: boots the Tectoy SDK and supplies a DeviceProfile
                    (vendor slot→subId lookup). All Tectoy-proprietary artifacts
                    live inside this module and are gitignored. See its README.

A new device = a new small application module implementing
`DeviceProfileProvider` on its Application. The harness never imports a vendor
library; `:device-tectoy` is the worked example of that separation.

## Prerequisites

- `libipacore.so` for the target ABI at
  `spike/src/main/jniLibs/<abi>/libipacore.so`. Produce it from the repo root:

      NDK_ROOT=/path/to/android-ndk-r27d \
        scripts/build-android-deps.sh armeabi-v7a /tmp/ipa-deps/armeabi-v7a

      cmake -S . -B build-android-armv7 \
        -DCMAKE_TOOLCHAIN_FILE=$NDK_ROOT/build/cmake/android.toolchain.cmake \
        -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-26 \
        -DCMAKE_FIND_ROOT_PATH=/tmp/ipa-deps/armeabi-v7a \
        -DOPENSSL_ROOT_DIR=/tmp/ipa-deps/armeabi-v7a
      cmake --build build-android-armv7 --parallel

      cp build-android-armv7/src/ipa/libipacore.so \
         android/spike/src/main/jniLibs/armeabi-v7a/libipacore.so

  The Tectoy terminal is 32-bit (`armeabi-v7a`). Add `arm64-v8a` similarly for
  64-bit gear (and to `abiFilters`).

- Android SDK + JDK 17. Gradle is vendored (`./gradlew`, Gradle 8.13, AGP 8.13.2).
- For `:device-tectoy`, the vendor SDK artifacts — see `device-tectoy/README.md`.

## Build & install

    # Generic (no proprietary deps) — quickest path to a running spike:
    ./gradlew :app-generic:assembleDebug
    adb install -r app-generic/build/outputs/apk/debug/app-generic-debug.apk

    # Tectoy-integrated (after dropping vendor artifacts into device-tectoy/libs/):
    ./gradlew :device-tectoy:assembleDebug
    adb install -r device-tectoy/build/outputs/apk/debug/device-tectoy-debug.apk

Launch **IPA eUICC Spike**, set the SIM slot, tap **Run eUICC spike**, read the
report (and **Copy report** to pull it off the device).

## Reading the result

- **"GET RESPONSE chaining WORKS … core's loop is viable"** — the transport
  exposes 61xx; the Phase 1 backend is good as-is.
- **"modem AUTO-ASSEMBLED … core loop will NOT fire"** — the modem returns the
  full body with SW=9000; confirm `euicc.c` tolerates a long single response
  (it should) and that no code path *requires* 61xx.
- **"FAILED to open the ISD-R logical channel"** — the app lacks carrier /
  system privilege for `iccOpenLogicalChannel`, or no eUICC is present. Install
  as a system/privileged app, or try the OMAPI fallback (a Kotlin-only swap
  behind the same C backend).

Not covered by this passive spike: proactive **REFRESH** (SW=91xx) and the
basic-channel **FETCH / TERMINAL RESPONSE** STK path — those need a real profile
enable/disable and are the remaining on-hardware item after this.

## Privileges — two gates, not one

Validated on hardware during the Phase-1 spike. Opening the ISD-R channel needs
**both** of these, and the second one surprises people:

1. **`MODIFY_PHONE_STATE`** (signature|privileged), for
   `iccOpenLogicalChannel` / `iccTransmitApduLogicalChannel`. Satisfied by a
   system/priv-app install; see `privileged-install/`. A plain side-load is
   denied with a `SecurityException` — debuggable is *not* privileged.

2. **Being the device's LPA.** Android checks that the calling package is the
   one `EuiccConnector.findBestComponent()` selects, i.e. a system app
   declaring an `android.service.euicc.EuiccService` guarded by
   `BIND_EUICC_SERVICE`. Carrier privileges and `WRITE_EMBEDDED_SUBSCRIPTIONS`
   do **not** substitute — without it you get
   "The calling package is not allowed to access ISD-R".

   `:spike` declares `SpikeEuiccService` (a stub that exists purely to win that
   selection, at intent-filter priority 1000) and both apps inherit it through
   manifest merge. Subclassing `EuiccService` needs the `@SystemApi` stubs in
   `spike/libs/` — `compileOnly`, never packaged.

One more hardware finding worth repeating: the modem's power-on TERMINAL
CAPABILITY does not reliably advertise device-LPA support. A Thales "GTO" eUICC
answered SW=6985 to every ES10 command until TERMINAL CAPABILITY was re-sent, so
`EuiccChannel.openChannel` sends it explicitly on the basic channel.
