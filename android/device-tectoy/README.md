# device-tectoy

Device-specific application module for the **Tectoy POS terminal**. It boots the
Tectoy hardware SDK and supplies a `DeviceProfile` to the reusable `:spike`
harness; everything else (the eUICC transport, the native core, the spike UI)
comes from `:spike` and knows nothing about Tectoy.

This module is the *only* place Tectoy-proprietary bits live, which is what keeps
the rest of the project device-agnostic ("just one possible device").

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

`libipacore.so` is provided by the `:spike` library (see the top-level
`android/README.md`); you do **not** copy it here.

## If the SDK is absent

The module still compiles nothing without the AAR (the vendor classes won't
resolve). Once the artifacts are in place it builds normally. At runtime, if the
SDK fails to connect, `TectoyDeviceProfile` degrades gracefully to the AOSP
`SubscriptionManager` slot→subId path — the spike still runs.
