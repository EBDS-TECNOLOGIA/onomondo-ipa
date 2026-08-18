# device-tectoy

Device-specific application module for the **Tectoy POS terminal**. It boots the
Tectoy hardware SDK and supplies a `DeviceProfile` to the reusable `:core`
library; everything else (the eUICC transport, the native core, the IPAd UI and
the spike diagnostic) comes from `:core`/`:ipad` and knows nothing about Tectoy.

This module is the *only* place Tectoy-proprietary bits live, which is what keeps
the rest of the project device-agnostic ("just one possible device").

> **Do not install this module as a system priv-app.** The vendor AAR merges 56
> permissions, about half of them `signature|privileged`, and a priv-app holding
> even one permission that is not in an allowlist stops the device from booting
> (confirmed on the terminal, 2026-08-18). Install `app-generic` privileged
> instead -- it needs three allowlist entries and reaches the eUICC through
> stock AOSP telephony on this very device, which is how the Phase-1 spike
> passed. Sideloading `device-tectoy` with `adb install` is fine and harmless;
> it simply cannot open the ISD-R. See
> [`../privileged-install/README.md`](../privileged-install/README.md) for the
> allowlist procedure and the boot-loop recovery.

## Required vendor artifacts (not committed)

The Tectoy SDK is vendor-licensed and **gitignored** (`libs/`). Before building,
copy these from the SDK drop (`Tectoylib-1.7.8/`) into `device-tectoy/libs/`:

    tectoylib-1.7.8.aar          (or the .aar version you were given)
    NeptuneLiteApi_V4.17.00_*.jar
    TectoyKernels2.2.12.jar
    PayLib-release-1.4.124.aar
    sunmi-library-release.1.0.24.aar

The build wires them in via `fileTree(dir: 'libs', ...)`. Adjust the exact file
names in `build.gradle`/here if your SDK drop differs. If the SDK also ships
loose per-ABI `.so` files (e.g. `Tectoylib-1.7.8/jniLibsPax|jniLibsNexgo` or the
demo's `app/armeabi-v7a/`) and the terminal needs them, drop them under
`src/main/jniLibs/armeabi-v7a/` in this module.

## Native core

`libipacore.so` is provided by the `:core` library (see the top-level
`android/README.md`); you do **not** copy it here.

## If the SDK is absent

The module still compiles nothing without the AAR (the vendor classes won't
resolve). Once the artifacts are in place it builds normally. At runtime, if the
SDK fails to connect, `TectoyDeviceProfile` degrades gracefully to the AOSP
`SubscriptionManager` slot→subId path — the spike still runs.
