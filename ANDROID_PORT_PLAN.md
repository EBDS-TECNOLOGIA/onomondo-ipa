# Android Port — Implementation Plan

**Project:** Onomondo IoT IPA (SGP.32)
**Scope:** Add Android as a build target, delivered as both a system daemon and an APK. Configuration is loaded from a JSON file (replacing the Linux command-line flags). The daemon writes the log to a size-limited rotating file; the APK displays the log live while active.
**Date:** 2026-08-03

---

## Verdict

The daemon is very feasible. The APK is feasible to build and run, and — because it ships as a **system/signature app on AOSP/OEM firmware** driving the **device's built-in eSIM** — the eUICC-access policy gate that would block an ordinary app is satisfied by the platform signature. Everything else requested (JSON config, rotating log file, on-screen log) is small, self-contained work.

The code side is easy because the codebase is already cleanly layered: all OS-specific surface is confined to three files, and the core is portable C99.

---

## What is already portable

| Layer | Files | Android status |
|---|---|---|
| Core protocol logic | `libipa/` (ES10x, ESipa, procedures) | Pure C99, no file I/O, no threads, no fork/daemon — all confined to `main.c`. Cross-compiles with the NDK unchanged. |
| ASN.1 codec | `libasn/` (asn1c-generated) | Portable C; `_DEFAULT_SOURCE`/Bionic quirks are minor. |
| Crypto | `http.c` -> OpenSSL | Android's BoringSSL is OpenSSL-family, so it satisfies the `CURLOPT_SSL_CTX_FUNCTION` requirement from the earlier TLS work (unlike GnuTLS). |
| HTTP | `http.c` -> libcurl | Standard NDK build. |
| JSON | `esipa_json.c` -> jansson | Already a dependency, so config parsing needs no new library. |

The whole core talks to the outside world through two narrow abstractions: `ipa_scard_*` (5 functions, all `void*`) and `ipa_http_*`. The core only ever calls `ipa_scard_transceive/init/free` and `ipa_http_*`; it never touches PC/SC or curl directly. That is the seam we port against.

---

## Target topology

One shared native core (`libipa` + `libasn` + `http` + a new Android `scard` backend), consumed by two front-ends:

```
                 +---------------------------------------------+
                 |  libipacore.so  (NDK-built, C99)            |
                 |  libasn . libipa . http(BoringSSL+curl)     |
                 |  scard_android.c  <- new backend            |
                 |  log.c (sink-abstracted)   jansson config   |
                 +----------------+--------------+-------------+
                        JNI        |              |  JNI
              +--------------------+              +------------------+
    +---------v----------+                          +---------------v----------+
    | Daemon             |                          | APK (system/signature)   |
    | system service     |                          | Foreground Service + UI  |
    | JSON cfg +          |                         | JSON cfg + live log view  |
    | rotating file log   |                         | (reads native ring buffer)|
    +--------------------+                          +--------------------------+
```

Both front-ends reach the eUICC through the same Android telephony path, since the app ships as a system/signature app on AOSP/OEM firmware.

---

## Three design decisions that drive everything

### 1. eUICC transport = TelephonyManager logical channel (recommended); OMAPI as fallback

As a system app you hold `MODIFY_PHONE_STATE`, so `iccOpenLogicalChannel(AID)` / `iccTransmitApduLogicalChannel(...)` / `iccCloseLogicalChannel()` are available with no SE-side ARA/ARF provisioning to arrange. OMAPI (`android.se.omapi`) is the alternative and maps more cleanly to raw APDUs (`Channel.transmit(byte[])` corresponds directly to `ipa_scard_transceive`), but its access is gated by an Access Rule Applet you would have to provision. Recommendation: TelephonyManager first; keep OMAPI as a compile-time-selectable backend if a device's telephony stack misbehaves.

### 2. The framework owns the channel — so the core must stop managing it

Both Android APIs perform MANAGE CHANNEL and the ISD-R SELECT internally, and they police the CLA channel bits. The core currently does both itself (`euicc.c`: `manage_channel()` sends INS 0x70, `select_isd_r()` sends the SELECT, and every APDU ORs the channel number into its CLA byte).

Resolution: add a transport capability flag `scard_manages_channel` (queried from the backend). When set, `ipa_euicc_init_es10x()` skips `manage_channel()` / `select_isd_r()`, and the per-APDU CLA channel-bit OR is neutralized (the transport rewrites CLA to the framework-assigned channel). This is a small, contained change at one seam — the raw ES10x transceive logic is untouched, and the Linux PC/SC path stays byte-for-byte unchanged behind the flag.

### 3. Logging funnels through one function

`ipa_logp` in `log.c` is the only log path, and it currently hardcodes `fprintf(stderr, ...)`. Introduce a pluggable sink: the daemon installs a rotating-file sink, the APK installs a ring-buffer sink the UI polls. No call sites change.

---

## Phase 0 — NDK build

**Status: done (build scaffolding).** The CMake side is in place and the native
(Linux/PC-SC) build is unaffected — verified building clean with 9/9 tests
passing. What landed:
- `IPA_TARGET_ANDROID` option in the top-level `CMakeLists.txt` (auto-on under
  the NDK toolchain, i.e. `CMAKE_SYSTEM_NAME=Android`; guarded so it errors if
  forced on without the toolchain, warns if the API level is < 26).
- `asn1c` is resolved on the **host** under the cross toolchain
  (`NO_CMAKE_FIND_ROOT_PATH`) since it is a host code generator.
- `src/ipa/CMakeLists.txt` branches: on Android it builds `libipacore.so`
  (whole-archive `libipa` + `libasn` + `http` + `scard_android.c`, BoringSSL +
  libcurl), drops the `pcsclite` link, and does **not** build the `ipa` CLI
  executable (`main.c` stays the Linux entry point). Host-run tests are skipped
  under cross-compile.
- `src/ipa/scard_android.c` added as a Phase-0 stub (not-yet-implemented
  eUICC backend) so the `.so` links with the full core + net + crypto and *no
  eUICC yet* — Phase 1 fills in the JNI bridge.
- Confirmed no `<PCSC/*>` / `<wintypes.h>` / `<winscard.h>` leak outside
  `scard.c`, so the core is Bionic-clean once `scard.c` is swapped out.

**Full link verified (arm64-v8a).** Beyond the scaffolding, an actual NDK cross
build was run end-to-end with `android-ndk-r27d`:
- Dependencies: OpenSSL 3.5.7 (libs only) + curl 8.21.0 (static, `--without-zlib`)
  cross-built for `arm64-v8a` into a deps prefix, using the release tarballs
  with the Configure flags from `ibaoger/libcurl-android`. (That repo's own
  flow needs `autoreconf`, absent here; release tarballs ship a pre-generated
  `configure`/`Configure` so only `perl` + `make` are required.) This is now
  captured as a reusable, per-ABI helper: `scripts/build-android-deps.sh`.
- `cmake -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake
  -DANDROID_ABI=<abi> -DANDROID_PLATFORM=android-26
  -DCMAKE_FIND_ROOT_PATH=<prefix> -DOPENSSL_ROOT_DIR=<prefix>` then
  `cmake --build` produced **`libipacore.so`** for **both target ABIs**:
  `arm64-v8a` (AArch64 `DYN`, ~11 MB) and `armeabi-v7a` (ARM/ELF32 `DYN`,
  ~9 MB), each with SONAME `libipacore.so`.
- Verified on both: `NEEDED` is only Bionic (`libm`/`libdl`/`libc`) — OpenSSL
  and curl are statically embedded (`SSL_connect`, `EC_KEY_new`,
  `curl_easy_init` present); the public `ipad.h` API is exported (`ipa_init`,
  `ipa_poll`, `ipa_execute_fallback`, `ipa_get_connectivity_params`,
  `ipa_set_default_dp_addr`, `ipa_scard_init`, …). This is the Phase-0
  deliverable: core + net + crypto linked into a `.so` for both ABIs, no eUICC
  yet.

Remaining for a fuller build later (not Phase 0): jansson (Phase 2) supplied the
same way via `CMAKE_FIND_ROOT_PATH`.

- Use the NDK's CMake toolchain file; parameterize `ABI` / `minSdk` (target `arm64-v8a` + `armeabi-v7a`, minSdk >= 26 for OMAPI / stable telephony APDU APIs).
- Provide Android builds of dependencies: BoringSSL (satisfies the `CURLOPT_SSL_CTX_FUNCTION` requirement — OpenSSL-family, unlike GnuTLS), libcurl, jansson. Fetch via an `ExternalProject` superbuild or prebuilts.
- New top-level option `IPA_TARGET_ANDROID` that: selects `scard_android.c` instead of `scard.c`, drops the `pcsclite` link, and builds `libipacore.so` (shared) instead of the `ipa` executable.
- Bionic vs glibc: `_DEFAULT_SOURCE` is already project-wide; verify no `<PCSC/*>` / `<wintypes.h>` leaks in when `IPA_TARGET_ANDROID` is set.
- **Deliverable:** core + net + crypto compile and link into a `.so` for both ABIs. No eUICC yet.

---

## Phase 1 — Android scard backend (the real work)

**Status: implemented AND validated on real hardware (2026-08-13).** The Phase-1
transport spike ran end-to-end against a Thales "GTO" SGP.32 eUICC on the Tectoy
POS terminal (armeabi-v7a): ES10c GetEID / ES10b GetEUICCInfo1 / GetEUICCInfo2 all
succeeded (real EID + EUICCInfo2 with a test-profile label returned). **Result on
the gating risk (positive):** the modem EXPOSES ISO 61xx chaining — STORE DATA
returns `61xx` and `GET RESPONSE` over the logical channel assembles the full body,
so the core's own `recv_es10x_block` loop is viable as-is; the modem does NOT
auto-assemble. **Key finding:** the transport MUST send TERMINAL CAPABILITY on the
basic channel (`iccTransmitApduBasicChannel`, `80 AA 00 00 05 A9 03 84 01 01`)
during channel setup — a phone modem's power-on TERMINAL CAPABILITY did not
advertise device-LPA support and the eUICC rejected all ES10 with SW=6985 until it
was re-sent (now done in `EuiccChannel.openChannel`). **Access model finding:**
ISD-R access on Android is reserved for the LPA (`iccOpenLogicalChannel` identity
check vs `EuiccConnector.findBestComponent`), so the app had to register a stub
`EuiccService` and be installed as a privileged system app; see `android/` and
`android/privileged-install/`. Remaining owed: proactive REFRESH (SW=91xx) /
basic-channel FETCH / TERMINAL RESPONSE, which need a real profile enable/disable.

What landed:
- Core seam (`scard_manages_channel`): new `ipa_scard_manages_channel()` in the
  `scard.h` contract — PC/SC returns false, Android returns true. In `euicc.c`
  a helper `es10x_channel()` returns 0 (basic channel) when the transport owns
  the channel so the CLA channel-bit OR is neutralized, and
  `ipa_euicc_init_es10x()` / `ipa_euicc_close_es10x()` skip
  termcap/MANAGE CHANNEL/SELECT-ISD-R in that case. The Linux PC/SC path is
  unchanged (verified: clean build, 9/9 tests). The four test mocks gained the
  new contract function.
- `src/ipa/scard_android.c`: real backend — opens/transmits/closes over JNI to
  a Java `EuiccChannel`, with thread attach/detach and Java-exception→`-EIO`.
- `src/ipa/scard_jni.c` + `src/ipa/scard_android.h`: JNI bridge (`JNI_OnLoad`
  caches the `JavaVM`; `EuiccChannel.nativeRegister/Unregister` hand the
  instance + method IDs to native).
- `src/ipa/android/EuiccChannel.kt`: `TelephonyManager` logical-channel wrapper
  (`openChannel`/`transmit`/`closeChannel`); APDU decomposition + CLA-bit
  clearing live here, so the C backend stays raw-bytes-in/out (OMAPI-swappable).
- Verified by cross-linking `libipacore.so` (arm64-v8a): `scard_android.c` +
  `scard_jni.c` compile against the NDK `jni.h`, and `JNI_OnLoad` /
  `Java_com_onomondo_ipa_EuiccChannel_nativeRegister` / `...nativeUnregister`
  are exported.

**Still owed (the on-hardware spike, unchanged as the gating risk):** confirm
`iccTransmitApduLogicalChannel`'s 61xx GET RESPONSE behaviour against a real
eSIM. The core drives its own 61xx loop and `EuiccChannel.transmit()` passes the
framework response through verbatim; if a modem/RIL auto-assembles 61xx and
returns the body with SW=9000 on the triggering command, the core would lose
data. Also unresolved on-device: FETCH / TERMINAL RESPONSE for a proactive
REFRESH (SW=91xx) are basic-channel STK APDUs that this logical-channel
transport cannot route — expected to be handled by the modem, to be confirmed.
These must be validated before relying on the backend.

*Spike harness delivered (run it to close the 61xx item):* `src/ipa/android_spike.c`
adds a native diagnostic (`Java_..._EuiccSpike_nativeRunSpike`) built into
`libipacore.so` that opens the ISD-R channel through the production transport,
sends known ES10x commands (GetEID / GetEUICCInfo1 / GetEUICCInfo2), records
every raw APDU exchange and classifies the modem's 61xx behaviour. The Gradle
app that runs it and shows the report on screen lives in `android/`: a reusable,
device-agnostic `:spike` library (transport + native JNI + UI + a `DeviceProfile`
seam), a dependency-free `:app-generic` application (stock AOSP telephony), and a
`:device-tectoy` application that boots the Tectoy POS SDK and supplies its
`DeviceProfile` — all vendor-proprietary artifacts confined to that one module.
The passive spike still does not exercise REFRESH (91xx) / FETCH / TERMINAL
RESPONSE; that remains for a profile enable/disable on hardware.

New file `src/ipa/scard_android.c` implementing the existing 5-function contract in `scard.h`:

| Function | Android implementation |
|---|---|
| `ipa_scard_init(reader_num)` | JNI up-call to open the logical channel to the ISD-R AID (`A0000005 59101002...`, from `select_isd_r` in `euicc.c`). Stores the framework channel + JVM handles in the ctx. `reader_num` maps to SIM slot index. |
| `ipa_scard_transceive(ctx,res,req)` | Marshal `req` bytes to the JVM; TelephonyManager path decomposes into cla/ins/p1/p2/p3/data and rewrites the CLA channel bits, OMAPI path passes raw; copy response (incl. SW) back into `res`. |
| `ipa_scard_reset` / `ipa_scard_atr` | Stubs — never called by the core (verified). Reset returns success; ATR returns empty, or `Session.getATR()` on the OMAPI path. |
| `ipa_scard_free` | Close the logical channel, release JNI globals. |

Core-side change (single seam): add `scard_manages_channel` capability; make `ipa_euicc_init_es10x()` / `ipa_euicc_close_es10x()` and the CLA channel-bit OR conditional on it. Guard with the flag so the Linux PC/SC path is unchanged.

JNI bridge: `scard_jni.c` (native) + a small Kotlin/Java `EuiccChannel` class wrapping `TelephonyManager`. Handle: exception propagation (Java exception -> `-EIO`), thread attach/detach (the poll loop runs on a native thread, so `AttachCurrentThread`), and slot/subscription selection.

**Risk to validate early:** `iccTransmitApduLogicalChannel`'s CLA channel-bit handling and its treatment of GET RESPONSE / 61xx chaining — confirm the core's `recv_es10x_block` GET RESPONSE loop survives when the framework auto-handles 61xx. This is the single highest-risk item; prototype it against real hardware before building the rest.

---

## Phase 2 — JSON configuration

**Status: done (2026-08-14).** Implemented and unit-tested on the Linux build;
the Linux CLI is unchanged except for one additive flag. What landed:

- `src/ipa/libipa/config_json.c` + `include/onomondo/ipa/config_json.h` —
  parses the file into a `struct ipa_run_config`, which is `struct ipa_config`
  plus the auxiliary settings `main.c` used to keep as locals (`nvstate_path`,
  `initial_eim_cfg_path`, `euicc_memory_reset`, `one_euicc_pkg_only`) and the
  `log` block Phase 3 will consume.
- `src/ipa/libipa/run.c` — `ipa_run_from_config(const char *json_path)` and
  `ipa_run(struct ipa_run_config *)`: the same sequence `main.c` performs
  (load nvstate → create context → init → apply initial eIM config /
  memory reset / poll loop → write nvstate back), expressed once so the
  daemon (Phase 4) and the APK (Phase 5) share it. `ipa_run_stop()` leaves the
  poll loop; it is async-signal-safe, and the CLI wires it to `SIGUSR1`
  alongside the existing `running=false`.
- `src/ipa/libipa/fileio.c` — whole-file load/save helpers, so the CLI and the
  config-driven path read the nvstate and BER blobs through one
  implementation instead of two copies.
- `tests/config_json/` — unit tests for defaults, every key, partial
  configuration, comment keys, the shipped example, and 25 rejection cases.
  Clean under ASan + UBSan.
- `contrib/ipa-config.example.json` — annotated example; the test parses it,
  so it cannot drift away from the parser.

**Defaults live in one place.** `config_json.h` defines `IPA_DEFAULT_*` and
`main.c` now derives its `DEFAULT_*` macros from them, so the CLI and the
config file cannot disagree about what "unset" means.

**Parsing is strict.** An unknown key, a wrong JSON type, or an out-of-range
number is a hard error naming the offending key, not something ignored. On an
unattended device a typo would otherwise silently run the IPAd on a default
the operator never chose. The escape hatch is that any key starting with `_`
is ignored, so a file can carry `"_comment"` annotations despite JSON having
no comment syntax. Absent keys are not errors — they take the default, exactly
as an omitted CLI flag does.

**Not expressible in JSON, on purpose:** the ES10b one-shot triggers
(`-i/-F/-b/-X/-x/-G/-D`) and the profile-installation consent callback (`-a`).
The triggers are device-policy decisions a daemon makes through the `ipa_*`
API in `ipad.h` at the moment its own signals fire, not a startup setting; the
callback is deprecated (github issue #5) and is a function pointer.

**Build note:** jansson detection in `src/ipa/libipa/CMakeLists.txt` used to
rely on `pkg-config` alone, so on a machine with `libjansson-dev` installed but
no `pkg-config` binary the ESipa JSON binding was silently compiled out. A
`find_path`/`find_library` fallback was added; without jansson the config
loader now fails with a clear log line rather than starting on defaults.

### CLI flag -> JSON key mapping

| CLI flag | JSON key | Notes |
|---|---|---|
| `-t TAC` | `tac` | hex string, exactly 8 digits |
| `-e eimId` | `preferred_eim_id` | optional |
| `-r N` | `reader_num` | SIM slot on Android |
| `-c N` | `euicc_channel` | 0..19; ignored when the transport manages the channel |
| `-f PATH` | `initial_eim_cfg_path` | |
| `-m` | `euicc_memory_reset` | boolean |
| `-n PATH` | `nvstate_path` | |
| `-y NUM` | `esipa_req_retries` | |
| `-C` | `eim_cabundle` | |
| `-S` | `eim_disable_ssl` | boolean |
| `-I` | `eim_disable_ssl_verif` | boolean |
| `-E` | `iot_euicc_emu_enabled` | boolean |
| `-1` | `one_euicc_pkg_only` | boolean |
| `-R` | `refresh_flag` | boolean |
| (none) | `esipa_binding` | `"asn1"` (default) or `"json"` |
| (new) | `log.path` | rotating-file sink (Phase 3) |
| (new) | `log.max_size_bytes` | rotating-file sink; max 1 GiB |
| (new) | `log.max_files` | rotating-file sink; max 1000 |
| (new) | `-j PATH` | **CLI-only**: run from a JSON file. Takes over completely — the other flags are ignored, so there is never a second source of truth for the same setting. Lets the Linux build exercise the exact entry point the daemon and the APK use. |

---

## Phase 3 — Logging: sink abstraction + rotation

**Status: done (2026-08-14).** The Linux CLI's stderr output is byte-identical
— the golden-file `compare_stderr` test still passes untouched. What landed:

- `log.c` — `ipa_logp()` now formats the whole record (prefix + message) into
  one buffer and hands it to a registered sink, instead of two `fprintf`s
  straight to stderr. `ipa_log_set_sink(NULL)` restores the stderr default.
  A record longer than the 512-byte stack buffer is re-formatted on the heap
  rather than truncated. Incidental benefit: one `fwrite` per record means
  concurrent writers can no longer interleave a prefix with another thread's
  message.
- `log_file_sink.c` — rotating-file sink for the daemon.  `<path>` → `<path>.1`
  → `<path>.2` …, oldest deleted; `max_files` counts the live file. Opens with
  append and counts the existing size, so a restart neither loses the log nor
  bypasses the size cap. Each record is flushed as written — a daemon that dies
  mid-session is precisely what the log is for.
- `log_ring_sink.c` — fixed-capacity ring buffer for the APK. Drops oldest
  content **on record boundaries**, so a reader never receives the tail of a
  line whose start it never saw. `ipa_log_ring_sink_read()` is a destructive
  drain, which is what a UI that appends wants.
- `log_jni.c` (Android target only) — `nativeLogRingInit` / `nativeLogDrain` /
  `nativeLogRingFree`, plus `nativeLogFileInit` / `nativeLogFileFree`, on
  `com.onomondo.ipa.NativeBridge`. `nativeLogDrain` returns `null` on an idle
  poll so the UI can skip the append.
- `run.c` — installs the file sink from the config's `log` block before
  anything else is logged, and logs an "IPAd starting" session marker.
- `tests/log_sink/` — 12 cases: sink dispatch (including an over-long record),
  append-on-restart, rotation, generation cap, rotation disabled, `max_files=1`,
  init failure, ring drain/overflow/oversized-record/partial-read, and sink
  switching. Clean under ASan **and** ThreadSanitizer.

**Both sinks are mutex-protected**, because the APK's UI drains the ring from
one thread while the poll loop writes from another. `libipa` therefore links
`Threads::Threads`; on Android and modern glibc that resolves to nothing.

**Sinks install themselves.** `ipa_log_file_sink_init()` / `ipa_log_ring_sink_init()`
make themselves the active sink and the matching `_free()` restores stderr, so
a front-end never calls `ipa_log_set_sink()` directly. Only one is active at a
time; installing the second displaces the first.

**A failed log file is not fatal.** If the configured path cannot be opened,
`ipa_run()` complains loudly and carries on with the stderr sink. Logging is
diagnostics; refusing to provision the device because a log file is unwritable
would be the worse failure.

**One line necessarily escapes to stderr:** `config: loaded <path>`, emitted by
`ipa_config_json_load()` before the sink can exist — the sink's own path comes
from the file being loaded. Everything after that goes to the configured sink.

**Rotation defaults** live in `log_sink.h` (`IPA_DEFAULT_LOG_MAX_SIZE_BYTES` =
256 KiB, `IPA_DEFAULT_LOG_MAX_FILES` = 5) and are applied when `log.path` is
set but the limits are not. An explicit `"max_size_bytes": 0` still means "do
not rotate", for when logrotate or the platform owns the policy.

---

## Phase 4 — Daemon packaging

**Status: done (2026-08-14),** with one correction to the plan's premise. What
landed:

- `src/ipa/ipad_android.c` → the **`ipad`** binary (Android CMake target,
  alongside `libipacore.so`). A thin `main()` around `ipa_run_from_config()`:
  `-c PATH` for the config, `-i SECONDS` to repeat, `SIGTERM`/`SIGINT`/`SIGUSR1`
  for a graceful stop, `SIGPIPE` ignored so a dead eIM connection cannot kill
  the daemon.
- `src/ipa/run_jni.c` → `NativeBridge.nativeRun(String)` / `nativeStop()`, the
  entry point for the **app-hosted** daemon.
- `contrib/android/ipad.rc` — init service definition.
- `contrib/android/sepolicy/{ipad.te,file_contexts}` — its own SELinux domain,
  state directory and network access.
- `contrib/android/README.md` — deployment guide for both shapes.
- `tests/run_guard/` — the single-run guard, tested deterministically.

### Correction: a native init daemon cannot use the telephony transport

The plan offered "an init `.rc` service (system image) **or** an app-owned
foreground service's child" as equivalent options. They are not. Phase 1
established that reaching the ISD-R through `TelephonyManager` requires the
calling process to hold `MODIFY_PHONE_STATE` **and** to be the device's LPA —
both properties of an Android *package* — and the transport bridges up into a
Kotlin `EuiccChannel` over JNI. A native process launched by `init` has no JVM
and no package identity, so it cannot satisfy either.

So the **app-hosted foreground service is the supported shape** for the
telephony path, and `ipad` is for integrators supplying their own transport (a
vendor APDU channel, an OMAPI helper). Both are built and documented; the
README leads with this distinction so nobody deploys the wrong one.

### Scheduling: the core is run-to-completion, not resident

A poll cycle ends as soon as the eIM has nothing pending, so
`ipa_run_from_config()` returns rather than idling. Repeating it is a
scheduling decision, and it is deliberately **not** in the library:

- `ipad` takes `-i SECONDS` and sleeps between cycles (interruptible by the
  stop signal), which is right for an init-launched daemon.
- The APK must schedule itself via `WorkManager`/`AlarmManager` — a native
  sleep loop would be killed by Doze.

Baking a sleep loop into the core would have forced the wrong primitive on the
app.

### Single-run guard

`ipa_run()` now refuses a second concurrent run with `-EBUSY` instead of
letting two poll loops share the stop flag and the nvstate file. An Android
service that gets restarted is the realistic way to hit this.

### Two bugs found while building this

- **`nvstate_deserialize()` read out of bounds on a truncated nvstate file**
  (`memcpy` of `sizeof(*nvstate)` with no length check; the following
  `len - sizeof(*nvstate)` underflowed). An empty file — exactly what a power
  cut during the nvstate save leaves on an IoT device — crashed the IPAd.
  Caught by ASan via the new test; now logged and treated as "start over".
  This was pre-existing and affected the Linux build too.
- **jansson was missing from the Android dependency script.** Since Phase 2 it
  is not optional: without it the config loader fails and neither front-end can
  start. `scripts/build-android-deps.sh` now builds it alongside OpenSSL and
  libcurl.

### SELinux

`ipad.te` grants the daemon its own domain, `/data/misc/ipa` and outbound
network access. It grants **no** eUICC access — the integrator adds the rules
for whatever transport they wire in. For the app-hosted shape the relevant
gates are the privileged-permission allowlist and the LPA declaration, not
sepolicy; see `android/privileged-install/`.

---

## Phase 5 — APK

- **Native:** the same `libipacore.so` via JNI. A `NativeBridge` (Kotlin) exposes `start(configPath)`, `stop()`, `pollLog()`.
- **Config:** ship/generate `config.json` in app storage; a settings screen edits it against the same JSON schema.
- **eUICC access:** `TelephonyManager` calls happen in Kotlin and are handed to native via the Phase-1 JNI channel object — as a signature/system app the `MODIFY_PHONE_STATE` grant is satisfied by the platform signature; declare it in the manifest and get the app onto the system image / signed with the platform key.
- **UI:** a foreground `Service` runs the poll loop off the main thread; a Compose screen tails the ring-buffer sink and renders log lines live while active. Start/stop controls map to the service.
- **Manifest/permissions:** `MODIFY_PHONE_STATE`, `FOREGROUND_SERVICE`, internet; privileged-permission allow-list entry for the system-app install.

---

## Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| Framework channel management vs. core's self-managed channel/SELECT + 61xx GET RESPONSE handling | High | Prototype Phase-1 transceive against real hardware first; gate core changes behind `scard_manages_channel` so Linux is untouched. |
| `iccTransmitApduLogicalChannel` masking/rewriting CLA bits | Medium | Neutralize the core's CLA channel OR in the Android transport; test SW=9000 round-trips. |
| SELinux denials for a system daemon reaching telephony | ~~Medium~~ **moot** | Resolved differently than expected: a native daemon cannot reach telephony at all (no JVM, no LPA package identity), so the telephony path runs in the app and sepolicy never enters into it. `contrib/android/sepolicy/` covers the native `ipad` daemon for integrator-supplied transports. |
| BoringSSL/curl/jansson NDK build friction | Low | Well-trodden; superbuild. |
| JNI threading (poll loop on native thread) | Low | `AttachCurrentThread` in the transport; cache `JavaVM*`. |

---

## Effort recap

- **Phase 0:** small–moderate.
- **Phase 1:** moderate–large + the one hardware-validation spike (dominant on the critical path).
- **Phases 2–3:** done.
- **Phase 4:** done.
- **Phase 5:** moderate (the Android UI/service is the bulk).

The Linux/PC-SC build stays fully intact throughout — every Android change is behind `IPA_TARGET_ANDROID` or a runtime capability flag.

**Critical-path recommendation:** do the Phase-1 transceive spike against a real eSIM before committing to the rest. The channel-management / GET-RESPONSE behavior of the Android telephony APIs is the only thing here that can force a redesign, and it is cheap to de-risk up front.
