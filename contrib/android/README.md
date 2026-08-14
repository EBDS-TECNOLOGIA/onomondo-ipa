# Deploying the IPAd on Android

Artifacts for **Phase 4** of [ANDROID_PORT_PLAN.md](../../ANDROID_PORT_PLAN.md).

## Read this first: which process can talk to the eUICC

The Phase-1 hardware spike established that reaching the ISD-R through
Android's `TelephonyManager` requires two things **of the calling process**:

1. the `MODIFY_PHONE_STATE` permission (signature|privileged), and
2. being the device's LPA — the package that wins
   `EuiccConnector.findBestComponent()`.

Both are properties of an Android *package*, and the transport bridges up into
a Kotlin `EuiccChannel` object over JNI. A standalone native process launched
by `init` has no JVM, no package identity, and therefore cannot satisfy either.
Carrier privileges and `WRITE_EMBEDDED_SUBSCRIPTIONS` do **not** substitute.

So there are two deployment shapes, and only one of them works with telephony:

| | Transport | Use when |
|---|---|---|
| **App-hosted service** (recommended) | Android telephony | You are driving the device's eUICC through `TelephonyManager`. |
| **Native `ipad` daemon** | Whatever you supply | You have a vendor APDU channel or an OMAPI helper and no need for the telephony path. |

## App-hosted service (the telephony path)

The daemon logic runs inside the LPA app's process, in a foreground `Service`
on a worker thread:

```kotlin
// Blocks for the whole poll cycle -- never call this on the main thread.
val rc = NativeBridge.nativeRun("/data/data/<pkg>/files/config.json")
```

`NativeBridge` (JNI, `src/ipa/run_jni.c` and `src/ipa/log_jni.c`):

| Method | Purpose |
|---|---|
| `nativeRun(String configPath): Int` | Run one poll cycle to completion. 0 on success, negative on error, `-EBUSY` if already running. |
| `nativeStop()` | Ask a running `nativeRun()` to leave its poll loop. |
| `nativeLogRingInit(Int): Int` | Install the in-memory log sink for the live log view. |
| `nativeLogDrain(): String?` | Log text since the last call; `null` when idle. |
| `nativeLogRingFree()` | Drop the ring sink. |
| `nativeLogFileInit(String, Long, Int): Int` | Install the rotating-file sink. |
| `nativeLogFileFree()` | Close the log file. |

The app must also be the LPA: declare an `android.service.euicc.EuiccService`
and install as a privileged app. The Phase-1 spike app under
[`android/`](../../android) does exactly this and is the working reference.

**Scheduling is the app's job.** A poll cycle ends as soon as the eIM has
nothing pending, so `nativeRun()` returns rather than idling. Re-run it from
`WorkManager` or `AlarmManager` — a native sleep loop would be killed by Doze.

## Native `ipad` daemon

Built as `ipad` by the Android CMake target, alongside `libipacore.so`.

```
usage: ipad [-c PATH] [-i SECONDS]
  -c PATH      JSON configuration file (default: /data/misc/ipa/config.json)
  -i SECONDS   re-run every SECONDS instead of exiting after one poll cycle
  -h           print this text
```

With `-i` it sleeps between cycles and keeps going across failures; without it,
it runs once and exits with a non-zero status on error. `SIGTERM`, `SIGINT` and
`SIGUSR1` all request a graceful stop — the poll loop finishes the eIM request
and eUICC exchange in flight, then returns, so pair `SIGTERM` with a timeout if
you need a hard deadline.

### Install

| File | Destination |
|---|---|
| `ipad` | `/system/bin/ipad` |
| `ipad.rc` | `/vendor/etc/init/ipad.rc` (or `/system/etc/init/`) |
| `sepolicy/ipad.te`, `sepolicy/file_contexts` | your `BOARD_SEPOLICY_DIRS` |
| your `config.json` | `/data/misc/ipa/config.json` |

`ipad.rc` creates `/data/misc/ipa` on `post-fs-data` and starts the service on
`sys.boot_completed`, so it neither races `/data` being decrypted nor respawns
in a tight loop when the eIM is unreachable.

The supplied `ipad.te` grants the daemon its own domain, its state directory
and outbound network access. **It grants no eUICC access** — add the rules for
whatever transport you wire in.

### Configuration

Same JSON file as everywhere else — see
[`contrib/ipa-config.example.json`](../ipa-config.example.json). For a daemon,
set `log.path` so the log goes to a rotating file rather than to a stderr
nobody is reading:

```json
{
  "nvstate_path": "/data/misc/ipa/nvstate.bin",
  "log": { "path": "/data/misc/ipa/ipa.log", "max_size_bytes": 262144, "max_files": 5 }
}
```

One line still reaches stderr before the sink exists: `config: loaded <path>`.
That is unavoidable — the sink's path comes from the file being loaded.
