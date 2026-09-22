# onomondo-ipa on OpenWrt — Port Analysis

*Tree: `~/openwrt-port/ipa`, branch `ports/openwrt`. 2026-09-17, updated
2026-09-21. Android daemon pieces ported forward (§1.1); daemon, OpenWrt
packaging and ubus implemented, LuCI dropped (§1.2); the mbedTLS backend and the
AT+CSIM modem transport implemented (§1.2, §9.4).*

## 0. Summary

The IPAd core (`libasn` + `libipa`) is portable C and needs almost nothing to
build for OpenWrt. **The port is the platform around it**, and three problems
dominate:

1. **eUICC access goes through the modem, not a smart-card reader.** The core
   was written against a raw PC/SC-style APDU pipe. It opens the logical
   channel, selects the ISD-R, sends TERMINAL CAPABILITY, chains GET RESPONSE,
   answers STK proactive commands and resets the card itself. Modems expose
   APDU access in four different ways (AT+CSIM, AT+CCHO/CGLA, QMI UIM, MBIM
   UICC low-level access), and in each one the modem already does some of that
   work. The transport layer has to become a **pluggable backend with declared
   capabilities**, and `euicc.c` has to consult those capabilities. This is the
   largest source change and the largest hardware unknown.
2. **The IPA uses the WAN link that it manages.** Enabling a profile
   re-attaches the modem, can change the APN and drops the HTTPS session. A
   router has a connection manager (netifd with a `qmi`/`mbim`/`ncm`/`3g`
   protocol handler, or ModemManager) that owns the modem ports. The daemon
   must work *with* it. That means not fighting over the AT port, waiting for
   the interface to come back, and feeding registration state (RPLMN,
   registration failure → fallback) into `libipa`.
3. **It must run headless.** Today `ipa` is a run-to-completion CLI that
   prints to stderr and asks for consent on stdin. The router needs a procd
   daemon with UCI configuration, logging to syslog and/or rotating files, a
   power-safe nvstate, and control via signals/ubus. The web UI (LuCI)
   integration comes on top of that. The groundwork (JSON config,
   `ipa_run()` loop, log sinks) has been ported forward from `ports/android`
   (§1.1).

Secondary items: OpenWrt's libcurl uses **mbedTLS** by default, while `http.c`
needs OpenSSL. Cross-building needs a **host asn1c**. The CMake files leak
host paths (`/usr/include/PCSC`). **musl**, big-endian MIPS and flash wear
need care.

To keep it generic, the port is organised in layers that an integrator adapts
**by data, not by code**: transport backend (C, a handful) → modem profile
(data file with AT quirks) → board defaults (UCI) → connection-manager adapter
and hook scripts (shell).

## 1. Starting point

| Item | State |
|---|---|
| Branch | `ports/openwrt`, identical to `sgp.32-v1.2` HEAD (`021b5e8`), 0 commits of its own |
| Core | SGP.32 v1.2, both ESipa bindings, 17/17 tests in the default configuration (per `MIGRATION_STATUS.md`) |
| Platform modules | `src/ipa/scard.c` (pcsc-lite), `src/ipa/http.c` (libcurl + OpenSSL), `src/ipa/main.c` (CLI) |
| Daemon support | **Ported forward from `ports/android`** (§1.1): `config_json.c`, `run.c` (`ipa_run_from_config`), `log_sink.h` with rotating-file and ring sinks, `fileio.c`. The transport capability query `ipa_scard_manages_channel()` was left for the transport phase (nothing in the core uses it yet). |
| Stray files | Untracked `android/` directory (only `.gradle/`, `local.properties` and empty `src/` trees), plus `g-eim*.{ber,json,pem}` test material in the tree root. Not part of this port; I'd delete `android/`. |

### 1.1 Android daemon pieces carried forward (2026-09-17)

`ports/android` forked from an older core (it has 13 commits that are not on
this branch, and this branch has 69 that are not on it). So the pieces were
**ported, not merged**, and arranged so that a later consolidation merge
conflicts as little as possible:

* **Copied byte for byte:** `include/onomondo/ipa/{config_json,log_sink}.h`,
  `src/ipa/libipa/{config_json.c,fileio.c,fileio.h,log_file_sink.c,log_ring_sink.c}`,
  `tests/{config_json,log_sink}/`, `contrib/ipa-config.example.json`. These
  merge cleanly, since both sides add identical content.
* **Copied with one forced edit each:** `run.c` (this branch's
  `ipa_euicc_mem_rst()` takes a bitmask, and the same subsets as the CLI's
  `-m` are used) and `tests/run_guard/run_guard_test.c` (drops the stub for
  the mTLS scaffolding, which this branch does not have).
* **Shared files, with the Android hunks at the same positions:** `log.h`
  (sink API), `log.c` (sink table, and `ipa_logp()` now formats each record
  once and keeps this branch's level filter and optional source location),
  `libipa/CMakeLists.txt` (new sources, Threads, jansson probed on its own
  with the Android fallback and defining `IPA_HAVE_JANSSON`, so
  `config_json.c` needed no change), and `tests/CMakeLists.txt`.
* **Not carried:** Android-only files (JNI, `scard_android.c`,
  `ipad_android.c`, Gradle), the Android top-level CMake block, `README.md`
  text, and the unrelated log-level tweaks in `es10*.c`.
* **Result:** default and `-DESIPA_BINDING_JSON=OFF` builds have zero
  warnings; 25/25 tests pass under ASan (22 existing plus 3 ported). A trial
  merge of `ports/android` (in a scratch clone) shrinks the conflicts
  compared with merging the untouched branch: `log.h` 47→16 lines,
  `libipa/CMakeLists.txt` 56→11, `log.c` 259→151. Every remaining hunk in
  those files resolves by taking this branch's side. `run.c` and
  `run_guard_test.c` each add one small hunk.
* Behaviour change to note: when jansson is present, it is now linked even
  with `-DESIPA_BINDING_JSON=OFF`, because the configuration file needs it.

### 1.2 Implemented so far: daemon, packaging, ubus, LuCI (2026-09-17)

New files carry the EBDS copyright line.

| Piece | Where | Notes |
|---|---|---|
| Daemon | `src/ipa/ipad_linux.c` (target `ipad`, via `src/ipa/linux_daemon.cmake`) | Loop around `ipa_run_from_config()`; `-s` syslog, `-l`/`-d` log levels, `-S` status file, `-w` readiness command, `-e` event command; TERM/INT/USR1 stop, **USR2 polls now**, HUP re-reads the configuration and polls now |
| syslog sink | `libipa/log_syslog_sink.c`, `onomondo/ipa/log_syslog.h` | Level → syslog priority, subsystem kept as prefix |
| Run observer | `onomondo/ipa/run_observer.h`, hooks in `run.c` | Events after init, eIM init and each poll; the daemon blocks on `profile_changed` until the readiness command passes |
| Context info | `ipa_get_ctx_info()` in `ipad.h` | EID, eIM id/FQDN, IPA mode, for the status file |
| Power-safe nvstate | `fileio.c`, `run.c`, `ipad.c` | Temporary file + `fsync` + `rename`; skipped when unchanged. **Core fix:** `nvstate_serialize()` wrote heap pointer values into the image, so identical states never produced identical files; those slots are now written as zero (the deserializer never used them) |
| OpenWrt feed | `contrib/openwrt/ipad/`, `contrib/openwrt/luci-app-ipad/`, `contrib/openwrt/README.md` | Package Makefiles, procd init, UCI defaults, readiness/event scripts, NTP hotplug, sysupgrade keep list, rpcd plugin, LuCI status/settings/log pages with ACL |
| eUICC transport | `onomondo/ipa/scard_transport.h`, `src/ipa/scard_dispatch.c`, `src/ipa/scard_at.c` | `transport` in the configuration (`-T` for the CLI) picks PC/SC or a modem's AT+CSIM at run time. `scard.c` is untouched: the build maps its names to `ipa_scard_pcsc_*` |
| Channels | `libipa/euicc.c`, `ipa_config.euicc_channel` | `"auto"` (IPA_EUICC_CHANNEL_AUTO) lets the eUICC pick the logical channel, which a modem needs; channels 4 to 19 are encoded correctly in the CLA byte |
| TLS | `src/ipa/http_tls.h`, `http_tls_openssl.c`, `http_tls_mbedtls.c` | `-DIPA_HTTP_TLS=openssl|mbedtls`; the OpenWrt package follows libcurl's setting (§9.4) |
| Tests | `tests/daemon_support/`, `tests/http_tls/`, `tests/scard_at/` (ctest), `contrib/openwrt/test/rootfs-test.sh` | ctest: syslog sink, atomic/write-on-change save, daemon signals/status/hooks. Rootfs test: 39 checks against the real OpenWrt 25.12.5 userland (procd, ubus, rpcd, logd, uhttpd) in an unprivileged namespace |

Verification (2026-09-21): 31/31 ctest in the default, `-DESIPA_BINDING_JSON=OFF`
and `-DESIPA_BINDING_ASN1=OFF` builds, 32/32 with `-DIOT_EUICC_EMULATION=ON`, all
with ASan and zero warnings; the rootfs test passes 47/47. The TLS cases also ran
against Mbed TLS 3.6.7 with curl 8.19 built from source. **Not verified:** a cross
build with the OpenWrt SDK (its host prerequisites need `apt`, §10), and the
transport against a real modem — the AT test drives a fake modem on a pseudo
terminal, replaying a recorded Quectel EC200A session.

Findings during the work:

* **procd `reload_signal`** is sent *instead of* a restart when the instance
  changed, and nothing happens when it did not. That makes it unsuitable
  here. The init script registers the rendered JSON with
  `procd_set_param file` instead, so a reload restarts the daemon exactly when
  the effective configuration changed.
* **Merge footprint:** `src/ipa/CMakeLists.txt` differs by one `include()`
  line and merges cleanly. `linux_daemon.cmake` skips itself when the Linux
  targets are absent, as they are in the Android build. `fileio.c`,
  `fileio.h`, `run.c` and `run_guard_test.c` exist on both branches without a
  common ancestor, so any difference conflicts. This branch's versions are
  supersets, so resolve those conflicts by keeping them, or bring the same
  changes to `ports/android` before consolidating.

### 1.3 How the core touches the platform

| Seam | Header | Linux implementation | Used by |
|---|---|---|---|
| APDU transport | `include/onomondo/ipa/scard.h` | `scard.c` (PC/SC) | only `libipa/euicc.c` (`ipa_scard_transceive`, `ipa_scard_reset`) |
| HTTPS | `include/onomondo/ipa/http.h` | `http.c` (libcurl, OpenSSL `SSL_CTX` hook for the eUICC-provisioned trust anchor) | `libipa/esipa.c` |
| Logging | `include/onomondo/ipa/log.h` | `libipa/log.c` → `stderr` | everywhere |
| Persistent state | `ipa_new_ctx(cfg, nvstate)` / `ipa_free_ctx()` return an opaque blob | `main.c` `fopen("w")`+`fwrite` | the front end |
| Scheduling | `ipa_poll()` return codes (`AGAIN`, `AGAIN_LATER`, `AGAIN_WHEN_ONLINE`, `CHECK_SCARD`, `CHECK_HTTP`) | `main.c` exits on `AGAIN_LATER` | the front end |
| Device facts | `ipa_config.tac`, `.imei`, `.device_capabilities`, `ipa_set_rplmn()` | CLI flags / constants | the front end |
| Device policy | `ipa_execute_fallback()`, `ipa_return_from_fallback()`, emergency profile, `ipa_get_connectivity_params()` | one-shot CLI flags | the front end |

The seams are clean. Apart from the transport semantics in §3, the OpenWrt
work is new implementations behind existing headers plus a new front end.

## 2. Target system

```
 +------------------------------ OpenWrt router ------------------------------
 |
 |  LuCI (uhttpd + rpcd) --ubus--+
 |                               v
 |  procd --> ipad (daemon) -- libipa -- transport backend --+-- AT tty   (/dev/ttyUSBx)
 |              |   |   |                                    +-- QMI      (/dev/cdc-wdm0, qmi-proxy)
 |              |   |   +-- http (libcurl+TLS) --> WAN --+   +-- MBIM     (/dev/cdc-wdm0, mbim-proxy)
 |              |   +-- syslog/logd, rotating file       |   +-- PC/SC    (USB reader, dev only)
 |              +-- /etc/config/ipad (UCI), nvstate      |            |
 |                                                       |            v
 |  netifd (proto qmi|mbim|ncm|3g) or ModemManager ------+--> cellular modem -- eUICC
 |        ^  hotplug iface up/down                              (soldered MFF2 or slot)
 |        +------------------ ipad waits on / triggers -----------+
 +-----------------------------------------------------------------------------
                                  |  HTTPS (ESipa)
                                  v
                                 eIM --> SM-DP+ (ES9+), reached via the IPA
```

Assumptions for the device class, to be confirmed per device (§9):

* SoC: MIPS (ath79, ramips — often big-endian on ath79), ARMv7 or ARM64
  (mediatek/filogic, ipq40xx, qualcommax). 16–512 MB flash, 64–1024 MB RAM.
* Modem: LTE Cat-4…Cat-12 or 5G module (Quectel EC2x/EG0x/RM5xx, Sierra EM/MC,
  Telit LE910/LM960, SIMCom SIM7x00/SIM8200, Fibocom…), usually on USB (also
  when on an M.2/mPCIe slot). It exposes several ttys plus a QMI or MBIM
  control node. Some low-cost boards use a UART-only module.
* eUICC: SGP.32 IoT eUICC (target) or SGP.22 consumer eUICC (lab, through the
  existing emulation). Soldered or in the SIM slot.
* No RTC battery on most boards. Time comes from NTP over the same cellular
  link.
* Operated headless. Configured by the vendor web UI or LuCI.

## 3. eUICC access through the modem (the hard part)

### 3.1 Access paths

| Path | Standard | What the modem does for you | Typical availability | Notes |
|---|---|---|---|---|
| **AT+CSIM** | 3GPP TS 27.007 §8.17 | Nothing in principle: raw APDU on any channel | Most modules; **some restrict or disable it**, some reject MANAGE CHANNEL or SELECT through it | Closest to the current core. Some firmwares answer 61xx themselves, others don't. Hex doubles the size on the wire, so the AT line-length limit matters. |
| **AT+CCHO / AT+CGLA / AT+CCHC** | 27.007 §8.45–8.47 | MANAGE CHANNEL + SELECT by AID; **the modem allocates the channel** | Most LTE modules | What `lpac`'s `at` backend uses. Channel number and FCI are not under our control; the FCI is usually not returned. CLA channel-bit handling varies (the Android RIL on the Tectoy passed CLA through verbatim). |
| **QMI UIM** (Open Logical Channel, Send APDU, Power Off/On, Terminal capability) | Qualcomm proprietary | Channel management, optional auto GET RESPONSE | Qualcomm-based modules (the majority: Quectel, Sierra, Telit…) | Needs libqmi-glib (or uqmi). Shares the control node with netifd/ModemManager through `qmi-proxy`. `qmicli` 1.32 exposes power and slots but not the logical-channel calls; the library API does. `lpac` ships `qmi` and `uqmi` backends, which shows both work. |
| **MBIM MS UICC Low Level Access** | Microsoft MBIM extension | Open/close channel, APDU, **terminal capability**, **reset**, ATR | MBIM-mode modules (common for 5G M.2) | `mbimcli` 1.28 already exposes all of them, a strong sign that libmbim has it. Shares the node through `mbim-proxy`. |
| **PC/SC** (existing) | — | — | USB reader on the router (`pcsc-lite` + `ccid` are packaged) | Development and CI only; lets the daemon be validated on OpenWrt before the modem backend exists. |

Recommendation: implement **AT (both CSIM and CCHO/CGLA flavours) first**, since
that is what was asked and works on almost every module. Design the backend
interface so that **QMI and MBIM** fit behind it later. On a
Qualcomm/MBIM module that netifd drives via `qmi`/`mbim`, those backends avoid
fighting over a tty and are more robust than AT.

### 3.2 Where the modem breaks the core's assumptions

Each item points at the code that assumes otherwise.

1. **Channel ownership.** `euicc.c:manage_channel()` sends MANAGE CHANNEL for
   the fixed `cfg->euicc_channel`, and `select_isd_r()` sends SELECT
   (P2=`04`, expects SW `61xx`, then reads the FCI). With CCHO/QMI/MBIM, the
   modem opens the channel, selects the AID and hands back its own channel
   number. The core must skip both steps and learn the channel from the
   backend. `ports/android` already solved exactly this with
   `ipa_scard_manages_channel()`, so port that idea and generalise it (§3.3).
   The FCI (`parse_isdr_fci`, IPAe detection) becomes best-effort-absent,
   which the code already tolerates.
2. **TERMINAL CAPABILITY.** `send_termcap()` sends `80 AA 00 00 05 A9 03 84 01
   01` on the basic channel to declare IPAd support (SGP.32 §3.8.4). The modem
   sends its own TERMINAL CAPABILITY at power-on, which usually does **not**
   declare an IPA. On the Android spike the eUICC then refused all ES10 with
   `6985` until ours was re-sent. Through AT this needs AT+CSIM on channel 0,
   which some firmwares block. MBIM has a dedicated call; QMI has one on recent
   firmware. **Whether the modem lets us set this is the first pass/fail
   question for any hardware.**
3. **Response chaining.** The core expects `61xx` and issues GET RESPONSE
   (`recv_es10x_block`). Some modems resolve `61xx` internally and return data plus
   `9000` directly. **Latent bug:** `send_es10x_block()` discards the data
   part of a STORE DATA response. A modem that auto-chains would make every
   ES10x call look like "success, empty response". The backend must either
   present the raw `61xx` behaviour (buffer the data and serve synthetic GET
   RESPONSEs — keeps the core untouched) or the core must accept data on the
   last STORE DATA. I'd do the former in the backend, because it also covers
   modems that behave inconsistently.
4. **Block sizes and line length.** `MAX_BLOCKSIZE_TX` is 255, and a full
   APDU becomes ~530 hex characters on an AT line. Some firmwares cap the
   command line (often ~512–1024 characters) or the CGLA length. The
   transmit block size must become a backend parameter (`lpac` has
   `LPAC_CUSTOM_ES10X_MSS` for the same reason). There is precedent in the
   core: `MAX_GET_RESPONSE_CHUNK_RX` is already reduced for macOS.
5. **CLA channel bits.** The core ORs the channel into CLA
   (`STORE_DATA_CLA | channel`). CGLA/QMI/MBIM may overwrite, require or
   reject them. This has to be a per-modem quirk, and it is testable with the
   probe in §9.3.
6. **STK proactive commands.** On `91xx` the core issues FETCH and TERMINAL
   RESPONSE itself (`handle_proactive_refresh`). **On a modem, the modem's STK
   stack owns the proactive session.** A FETCH through AT+CSIM steals the
   command from it, or is rejected. The modem normally executes REFRESH itself
   (and may reset the card and all channels). The core needs a
   "proactive-handled-by-modem" mode: do not FETCH, treat `91xx` as `9000`,
   then wait for the SIM-ready indication (`+CPIN: READY`, `+QSIMSTAT`, QMI
   card-status indication…) and re-initialise the ES10x link.
7. **Card reset.** `ipa_euicc_reset_es10x()` needs a real UICC reset (eUICCs
   answer `6985` after a profile change until reset). Options by path: MBIM
   `uicc-reset`, QMI `SIM power off/on`, vendor AT (for example
   `AT+QSIMSTAT`/`AT+QSIMDET` do not reset; many vendors have a SIM power
   command). The last resort, `AT+CFUN=4`→`AT+CFUN=1` or `AT+CFUN=0/1`, also
   drops the data bearer. The reset command is therefore a **modem-profile
   entry**, and the daemon must expect the WAN to go down.
8. **Port contention.** netifd's `3g`/`ncm` handlers (via `comgt`) and
   ModemManager open AT ports; ModemManager probes and grabs *all* of them.
   Rules: use a tty nobody else uses (most modules expose two AT ports); with
   ModemManager, tag it `ID_MM_PORT_IGNORE` via a udev/hotplug rule or use the
   QMI/MBIM proxy path. Never share one tty between two AT parsers.
9. **Unsolicited results.** AT responses must be parsed with URCs
   (`+CREG`, `RING`, `+QIND`, …) interleaved, and with `+CME ERROR:` mapped to
   `-EIO`. The AT backend needs a small, strict line parser with per-command
   timeouts. A slow card plus a slow modem can take several seconds per STORE
   DATA during a BPP load.
10. **Multi-SIM.** Dual-slot modules route the eUICC through a slot selector
    (`AT+QUIMSLOT`, QMI switch-slot, …), so "which slot holds the eUICC" is
    configuration. Some modules also carry **their own eSIM/LPA feature** that
    talks to the ISD-R; it must be disabled or it will race with us.

### 3.3 Proposed transport architecture

* Keep `scard.h` as the only thing `libipa` sees, but turn it into a
  dispatcher over a backend table. Selection is by URI string instead of
  `reader_num`, for example: `pcsc:0`, `at:/dev/ttyUSB2`,
  `at-csim:/dev/ttyUSB3`, `qmi:/dev/cdc-wdm0`, `mbim:/dev/cdc-wdm0`.
* Each backend reports a **capability struct**, which `euicc.c` consults
  instead of assuming:
  `manages_channel`, `channel_number` (after open), `sends_termcap` / can
  send it, `handles_proactive`, `max_tx_block`, `max_rx_block`,
  `cla_channel_bits` (set / leave / clear), `can_reset`.
* The generic 61xx emulation (§3.2 item 3) lives in a shared helper that
  backends can opt into.
* **Modem profiles as data** (`/usr/share/ipad/modems/*.json` or UCI
  sections), matched by USB VID:PID and/or `AT+CGMM`. A profile holds the
  quirks from §3.2 plus AT command templates for reset, slot select, SIM-ready
  detection, IMEI, registration/RPLMN and disabling a built-in LPA. A new
  module then needs a profile file, not a code change, unless its APDU path is
  truly new.
* The AT and QMI/MBIM backends are ordinary Linux code. **They can be written
  and tested on this Debian box with a USB modem** before any
  cross-compilation.

## 4. Networking and device-policy interplay

| Concern | Why it matters on a router | What the daemon does |
|---|---|---|
| Profile switch drops WAN | `ipa_poll()` returns `IPA_POLL_AGAIN_WHEN_ONLINE`; `main.c` ignores that today | Wait for the WAN interface (`/etc/hotplug.d/iface`, `ubus call network.interface.wwan status`, or ModemManager state), with a timeout |
| New profile may need another APN | netifd's `wwan` has a fixed APN | Hook script (§7.4). The integrator decides whether to read the APN from the profile/operator mapping, or from `ipa_get_connectivity_params()` (`httpParams` is opaque in the spec, so treat it with care), and then `uci set … ; ifup wwan` |
| Rollback | `proc_euicc_pkg_dwnld_exec.c` already rolls back when the result cannot be delivered after a profile change | Give the reconnect enough time before declaring failure; make that timeout configurable |
| Fallback (SGP.32 §5.9.20/21) | Registration failure is only visible to the modem | Adapter watches registration (`AT+CEREG?`, QMI NAS, MM state). After N minutes unregistered → `ipa_execute_fallback()`; later → `ipa_return_from_fallback()`. Policy lives in UCI. |
| RPLMN (§5.14.5) | Required for roaming detection by the eIM | Feed `ipa_set_rplmn()` from `AT+COPS?` / NAS on every registration change |
| TAC / IMEI / capabilities | Currently CLI constants (`DEFAULT_TAC "12345678"`) | Read IMEI from the modem (`AT+CGSN`/QMI DMS); TAC = first 8 digits; RATs from the modem profile |
| Clock | Certificate validity check (`cert.c`) needs a trusted clock; no RTC; NTP needs the WAN | Poll only after `sysntpd` reports sync (`/etc/hotplug.d/ntp`, `ACTION=stratum`). Decide per product whether to build with `-DCERT_ALLOW_UNSET_CLOCK=ON` (needed if a device can boot with no usable profile) |
| Poll cadence | The core is run-to-completion | Loop with a configurable interval, plus "poll now" on SIGUSR2 or ubus |
| Data cost | Metered cellular link | ASN.1 binding (default) is the compact one; sensible interval; retries already configurable with backoff |

## 5. HTTPS / TLS

* OpenWrt's `libcurl` selects **mbedTLS by default** (`LIBCURL_MBEDTLS`;
  OpenSSL, wolfSSL and GnuTLS are alternatives). `http.c` installs the
  eUICC-provisioned trust anchor (`trustedPublicKeyDataTls`) through
  `CURLOPT_SSL_CTX_FUNCTION`. curl documents that option for OpenSSL,
  wolfSSL and mbedTLS, but the callback receives an `SSL_CTX*` for the first
  two and an `mbedtls_ssl_config*` for mbedTLS, and `http.c` is written
  against OpenSSL.
* Options:
  * **A — build curl with OpenSSL** (`CONFIG_LIBCURL_OPENSSL=y`, depends on
    `libopenssl3`). No code change, but it costs flash: libopenssl is on the
    order of 1.5–2 MB against a few hundred kB for mbedTLS. It also forces a
    custom curl build unless the firmware already ships OpenSSL.
    **Recommended for phase 1.**
  * **B — mbedTLS variant of the trust-anchor code** in `http.c`
    (`#ifdef`, same public header). This matches stock OpenWrt images and
    small-flash devices. **Recommended as a later phase** once the target's
    flash budget is known.
  * wolfSSL can use most of the OpenSSL code through its compat layer, but
    that is not worth it unless a firmware already ships it.
* The system trust store (`ca-bundle`/`ca-certificates` packages) is used for
  eIMs with public certificates. `eim_cabundle` points elsewhere for private
  PKI.
* `-I` / `eim_disable_ssl_verif` must stay a lab-only setting: expose it in
  UCI only behind an explicit "insecure" option.

## 6. Build and packaging

### 6.1 Toolchain route

* **OpenWrt SDK** for the router's exact release and target/subtarget (for
  example `openwrt-sdk-25.12.x-<target>-<subtarget>_gcc-…_musl.Linux-x86_64`).
  It builds our package plus any dependency from the feeds (curl with
  OpenSSL, jansson). Current stable is the **25.12** series (25.12.0 released
  March 2026, point releases since). **25.12 replaced `opkg` with `apk`**, so
  packages are `.apk` there and `.ipk` on 24.10 and older vendor firmwares.
* **Vendor firmware caveat:** many 4G routers run a vendor fork of OpenWrt
  (older release, older musl/gcc, sometimes uClibc on very old ones). There
  the **vendor's SDK/GPL tarball** is the only safe toolchain, because a
  package built with the upstream SDK will not load against the vendor's libc
  and library ABIs.
* **ImageBuilder** (optional) builds a full image with the package
  preinstalled, for production.
* A full buildroot is only needed if neither SDK exists. With **one CPU core
  and ~19 GB free**, this box would take many hours and be short on disk for
  it.

### 6.2 Package layout (custom feed, e.g. `feeds/iapyx/ipad/`)

* `Makefile` using `include $(INCLUDE_DIR)/cmake.mk`. Packages: `ipad`
  (daemon + init script + UCI defaults), optionally `libipa` and
  `luci-app-ipad`.
* `DEPENDS:=+libcurl +libopenssl +libjansson +libpthread` (+ `+libubus +libubox
  +libuci` if the daemon talks ubus/UCI natively; `+libqmi`/`+libmbim` for
  those backends; `+pcsc-lite` only for the PC/SC backend, as a build
  variant).
* `PKG_LICENSE:=AGPL-3.0-only`. Firmware that ships this must offer the
  corresponding source. The AGPL network clause is unlikely to be triggered
  by an IPA client, but it needs a compliance note in the product.
* Config files listed in `conffiles`. nvstate stored outside `/tmp` and
  listed in `/lib/upgrade/keep.d/ipad` so it survives sysupgrade (§7.3).

### 6.3 Changes the build system needs

| Issue | Where | Fix |
|---|---|---|
| `asn1c` must run on the **host**; the SDK has none | top-level `CMakeLists.txt` (`find_program(ASN1C_EXECUTABLE asn1c)`) | Either a host package (`HOST_BUILD` of asn1c in the feed) or pre-generated sources. `ports/windows` added `-DIPA_LIBASN_GEN_DIR=` for exactly this; port that option. The generated codec is architecture-independent. |
| Host header path leaks into the cross build | `src/ipa/CMakeLists.txt`: `include_directories(include /usr/include/PCSC)`, `pcsclite` linked unconditionally | Make PC/SC an option (`-DIPA_TRANSPORT_PCSC`), found via pkg-config `libpcsclite` in the sysroot; default OFF for OpenWrt |
| Backend selection | new | `-DIPA_TRANSPORT_AT=ON`, `…_QMI`, `…_MBIM` |
| `curl` linked by bare name | `src/ipa/CMakeLists.txt` | `find_package(CURL)` / pkg-config |
| `-g` forced | `src/CMakeLists.txt` | Harmless (OpenWrt strips), but let `CMAKE_BUILD_TYPE` govern it |
| jansson via pkg-config | `libipa/CMakeLists.txt` | Works in the SDK; `ports/android` added a `find_path` fallback, port it |
| `M32`, `ENABLE_SANITIZE` | top-level | Must stay OFF in the package |
| `ipad` front end | new target | See §7 |

### 6.4 Portability checks for router targets

* **musl:** no glibc-only calls were found in the core. `malloc_usable_size`
  (only under `MEM_EMIT_DEBUG`) exists in musl. `_DEFAULT_SOURCE` is fine.
  GNU named-variadic macros (`args...`) need GCC or Clang, which is what the
  SDK uses.
* **Big-endian (ath79) and strict alignment:** BER handling is byte-wise.
  `struct ipa_nvstate` is `__attribute__((packed))` and saved as a raw image,
  which is fine on one host but not portable between hosts, so an nvstate
  can't be moved between devices of different endianness. Compile with
  `-Waddress-of-packed-member` to be sure no member's address escapes. Run
  the unit tests under `qemu-mips` (big-endian) and `qemu-arm` user mode.
* **32-bit targets:** `time_t` is 64-bit on musl ≥ 1.2 (OpenWrt ≥ 21.02).
  `%ld` is used with asn1c `long` values, which is correct.
* **Memory:** BPP download buffers grow in RAM (`ipa_buf_realloc`). Profile
  packages are usually tens to a few hundred kB, which is acceptable on 64 MB
  devices. Measure peak with `-DMEM_EMIT_DEBUG=ON` once.
* **`sleep()` in `esipa.c` retry backoff** blocks the daemon's only thread.
  That's acceptable if signal handling is done around it, and it doesn't
  matter once retries are driven from the loop.

## 7. Daemon, configuration, logging

### 7.1 Front end

A new `ipad` binary (sharing code with the Android `ipad_android.c`/`run.c`
design):

* `procd`-managed (`/etc/init.d/ipad`, `USE_PROCD=1`, `procd_set_param respawn`),
  single instance (lock file), no fork of its own.
* Loop: wait for preconditions (SIM ready, WAN up, time synced) → `ipa_init`
  / `eim_init` → poll until `AGAIN_LATER` → sleep interval. Handle
  `AGAIN_WHEN_ONLINE`, `CHECK_SCARD` (re-open the transport, reset the modem
  path, back off) and `CHECK_HTTP` (back off, wait for WAN).
* Signals: `SIGTERM`/`SIGINT`/`SIGUSR1` stop cleanly and save nvstate (SIGUSR1
  keeps the CLI's meaning); `SIGUSR2` polls now; `SIGHUP` re-reads the
  configuration and polls now. A procd reload restarts the daemon when the
  rendered configuration changed (§1.2).
* ubus and LuCI: see §7.6.
* The existing CLI `ipa` stays for bench work, including the initial eIM
  configuration (`-f`), which can also be a ubus method.

### 7.2 Configuration

UCI is the native format and what LuCI edits, so use `/etc/config/ipad`. The
Android port's JSON parser (`config_json.c`) can be reused by having the init
script render UCI into a JSON file under `/var/run/ipad/`, which avoids a
libuci dependency in the core. Linking libuci directly in the daemon is the
alternative. *As implemented, see `contrib/openwrt/ipad/files/ipad.config`;
the original sketch:*

```
config ipad 'main'
	option enabled '1'
	option transport 'at:/dev/ttyUSB2'      # pcsc:N | at:DEV | at-csim:DEV | qmi:DEV | mbim:DEV
	option modem_profile 'auto'             # or a profile name
	option euicc_slot '1'
	option eim_binding 'asn1'               # asn1 | json
	option eim_id ''                        # preferred eIM, empty = first
	option cabundle '/etc/ssl/certs/ca-certificates.crt'
	option poll_interval '900'              # seconds
	option wan_interface 'wwan'
	option wan_timeout '300'                # seconds to wait after a profile change
	option require_ntp '1'
	option nvstate '/etc/ipad/nvstate.bin'

config fallback 'fallback'
	option enabled '0'
	option unregistered_timeout '600'

config log 'log'
	option syslog '1'
	option level 'info'                     # error | info | debug, plus per-subsystem overrides
	option file ''                          # e.g. /tmp/ipad.log or /mnt/usb/ipad.log
	option file_max_size '262144'
	option file_max_files '3'
```

Board defaults go in `/etc/uci-defaults/` of a board-specific package or
image, which is how a new router gets its transport and profile without
editing the daemon.

### 7.3 Persistent state

* The current save path (`main.c:save_nvstate_to_file`) is `fopen("w")` +
  `fwrite`. A power cut leaves a truncated file. `ports/android` fixed the
  read side, but the write side still needs **write-to-temp + `fsync` +
  `rename`**. Routers lose power routinely.
* Location: on the overlay (`/etc/ipad/`), not `/tmp` (tmpfs, lost at boot).
  The file changes rarely (state-change cause, emulation counters), so flash
  wear is negligible **as long as the daemon writes only when the content
  changed**. Add that check; today every exit writes.
* Keep it across sysupgrade (`/lib/upgrade/keep.d/ipad`). Losing
  `epr_seq_number` (emulation) makes a conforming eIM discard results.

### 7.4 Logging

* **syslog sink** (new, alongside the Android file and ring sinks):
  `openlog("ipad", LOG_PID, LOG_DAEMON)`, with LERROR→`LOG_ERR`,
  LINFO→`LOG_INFO`, LDEBUG→`LOG_DEBUG`. On OpenWrt this reaches `logd`
  (`logread`, LuCI's system log, and remote syslog if the user configured
  `log_ip`). Multi-line hexdumps should be one record per line.
* **Rotating file sink** (`ports/android` `log_file_sink.c`), default off. If
  on, point it at `/tmp` (RAM) or external storage, **never an unbounded file
  on flash**.
* Zero-code fallback: `procd_set_param stdout 1`/`stderr 1` forwards stderr to
  logd, which is useful for the very first on-device runs.
* Defaults: level `info` in production. `SHOW_ASN_OUTPUT` and debug hexdumps
  are too verbose for logd's ring buffer (typically 64 kB).
* **Hook scripts** in `/etc/ipad/hooks.d/` are run on events
  (`profile-changed ICCID`, `euicc-reset`, `poll-failed`, `fallback-entered`).
  This is where integrators adapt APN switching, LEDs or vendor UI
  notifications without touching C.

### 7.5 Headless policy decisions (build and config time)

| Decision | Mechanism | Suggested default for a router |
|---|---|---|
| Simple Confirmation for downloads | `prfle_inst_consent_cb`, NULL = auto-consent | NULL (auto) — the eIM is the authority |
| PPR end-user consent | `-DPPR_ALLOW_WITHOUT_CONSENT` | OFF (refuse); revisit if the eIM's RAT demands consent |
| Unset clock | `-DCERT_ALLOW_UNSET_CLOCK` | Product decision; ON only if a device may boot without a working profile |
| Consumer eUICC emulation | `-DIOT_EUICC_EMULATION` | OFF in production, ON in a lab variant |
| ESipa binding | runtime | ASN.1 unless the eIM requires JSON |

### 7.6 ubus and LuCI — in the first delivery, in the cheap form

*Superseded for the first target (§9.4): LuCI dropped; the rpcd plugin stays.*

Complexity depends almost entirely on **how** ubus is exposed:

| Approach | Effort | Why |
|---|---|---|
| **Native ubus object in `ipad`** (libubus/libubox, uloop) | High | `ipa_poll()` blocks for seconds to minutes (curl, slow modem APDUs), so serving ubus during a poll needs a uloop main thread plus a worker thread around a library that is single-context and not thread-safe. libubus/libubox are not packaged for Debian, so this is testable only inside OpenWrt (QEMU or hardware). |
| **rpcd exec plugin + status file** | Low | The daemon writes an atomic JSON status file (`/var/run/ipad/status.json`: state, EID, eIM, IPA mode, cycle counters, last result, next poll; profiles and active ICCID need a further library getter) at the end of each cycle, and reacts to signals. An executable in `/usr/libexec/rpcd/ipad` publishes `ipad.status`, `ipad.poll` (SIGUSR2), `ipad.reload` (`/etc/init.d/ipad reload`) and `ipad.log` (`logread -e ipad`) on ubus. No libubus in C, and the plugin is a shell script testable in QEMU. |
| **LuCI app** (`luci-app-ipad`, client-side JS) | Low | `form.Map` on `/etc/config/ipad` for the settings page; a status view calling the rpcd methods; a log view; `menu.d` + `acl.d` JSON. Roughly a few hundred lines, with no server-side code. |

**Recommendation: include ubus (rpcd plugin) and LuCI in the first delivery,
and postpone the native ubus object.** The cheap form covers observation,
configuration and "poll now". What it can't do well is **synchronous
device-policy commands with a result** (manual fallback, emergency profile,
memory reset, initial eIM configuration). In v1 these stay automatic (UCI
policy) or go through the CLI with the daemon stopped. They become native ubus
methods later, if a real need appears. The ubus layer is also the valuable part
for a **vendor web UI**: most vendor OpenWrt UIs call ubus/rpcd, so they could
use `ipad.status` even when LuCI is absent.

Prerequisite for testing: an OpenWrt image in QEMU (§10), since neither rpcd
nor LuCI runs on the Debian host.

## 8. Keeping it generic

Five layers, each replaceable without touching the ones below it:

| Layer | Artifact | Adapting to new hardware means… |
|---|---|---|
| 1. Core | `libasn`, `libipa` | nothing |
| 2. Transport backends | `src/ipa/transport/{pcsc,at,qmi,mbim}.c` behind `scard.h` | only a new APDU *mechanism* (rare) |
| 3. Modem profiles | `/usr/share/ipad/modems/<vendor>-<model>.json` | a new data file: AT templates, block sizes, CLA handling, reset method, built-in LPA disable |
| 4. Board defaults | `uci-defaults` in a board package | transport URI, slot, WAN interface name |
| 5. Connection-manager adapter and hooks | `/usr/lib/ipad/cm/{netifd,modemmanager}.sh`, `/etc/ipad/hooks.d/` | selecting an adapter; writing hook scripts for APN or UI |

Also keep to what upstream OpenWrt provides (procd, ubus, UCI, netifd, logd,
`cmake.mk`) so that a vendor fork differs only in the SDK used.

## 9. What must be known about the hardware

### 9.1 Router

| Question | Why | How to find out |
|---|---|---|
| Brand, model, hardware revision | Everything else | Label / `cat /tmp/sysinfo/model` |
| Upstream OpenWrt or vendor fork? Release? | Selects the SDK and the package format (apk/ipk) | `cat /etc/openwrt_release`, `ubus call system board` |
| Target/subtarget, CPU arch, endianness | SDK choice | `/etc/openwrt_release` (`DISTRIB_TARGET`, `DISTRIB_ARCH`) |
| libc and version | ABI compatibility | `ls /lib/ld-musl-*`, `ldd --version` |
| Flash size/free, overlay type, RAM | OpenSSL vs mbedTLS, logging location | `df -h`, `free`, `cat /proc/mtd` |
| Installed TLS libs, curl build | Whether option A in §5 is free | `apk list -I` / `opkg list-installed`, `curl -V` |
| RTC present? | Clock policy | `ls /dev/rtc*`, `hwclock -r` |
| Root access (SSH/serial), package installation allowed? | Development path | — |
| Serial console pinout | Recovery when a test bricks networking | Board docs / OpenWrt wiki |
| Web UI: LuCI or vendor? | Integration route for status/config | Browse it; `ls /www` |
| Who manages the modem: netifd proto (`qmi`, `mbim`, `ncm`, `3g`, `modemmanager`) or a vendor daemon? | Port contention, WAN control | `uci show network`, `ps` |
| Watchdog, power-loss behaviour | nvstate safety | — |

### 9.2 Modem and eUICC

| Question | Why |
|---|---|
| Module vendor/model/firmware (`ATI`, `AT+CGMM`, `AT+CGMR`) | Modem profile |
| Bus and USB VID:PID and composition (`lsusb`, `/sys/kernel/debug/usb/devices`) | Which ttys and whether QMI/MBIM exist |
| Which tty is AT and which of those netifd/ModemManager already use | Choose a free AT port |
| Supports AT+CSIM? CCHO/CGLA/CCHC? Max length? | Transport choice (§3.1) |
| Returns `61xx` or auto-chains? CLA bits passed or rewritten? | Backend capability flags |
| Can we send TERMINAL CAPABILITY (CSIM on channel 0, or vendor/QMI/MBIM)? | **Go/no-go** (§3.2 item 2) |
| How to reset/power-cycle the UICC without a full `CFUN` cycle? | Profile switching |
| STK: does the modem handle REFRESH? Which URC signals SIM ready again? | Proactive mode |
| Single/dual slot, slot-switch command, which slot holds the eUICC | Configuration |
| Built-in eSIM/LPA feature in the firmware? Can it be disabled? | Avoid a second LPA on the ISD-R |
| eUICC: vendor, SGP.32 or SGP.22, form factor, EID | Emulation or not |
| Pre-provisioned with eIM configuration? Bootstrap/provisioning profile and its APN? | First-boot flow |
| Which eIM, which binding, private or public TLS PKI | HTTP configuration |

### 9.3 Modem probe (run on the router or on a PC with the module)

With nothing else using the port (e.g. `picocom /dev/ttyUSB2`):

```
ATE1                                   echo on for readability
ATI ; AT+CGMM ; AT+CGMR ; AT+CGSN      identity, IMEI
AT+CPIN?                               SIM ready?
AT+CSIM=?                              CSIM supported?
AT+CSIM=20,"80AA000005A903840101"      TERMINAL CAPABILITY (IPAd) on channel 0; expect 9000
AT+CCHO="A0000005591010FFFFFFFF8900000100"
                                       open ISD-R; returns a session/channel id <n>
AT+CGLA=<n>,16,"81E2910003BF2000"      GetEUICCInfo1 with CLA=81 (channel 1)
AT+CGLA=<n>,16,"80E2910003BF2000"      same with CLA=80 — tells whether the modem sets the channel bits
AT+CGLA=<n>,22,"81E2910006BF3E035C015A" GetEID
   -> response ends in 61xx? then:  AT+CGLA=<n>,10,"81C00000xx"   (auto-chaining if data+9000 instead)
AT+CCHC=<n>                            close
```

A `6985` on the ES10 calls after a successful open means the TERMINAL
CAPABILITY did not take effect. That was the failure seen on Android. If
`AT+CSIM` is refused, repeat the channel tests with `AT+CSIM` and an explicit
`MANAGE CHANNEL` (`0070000001`) to evaluate the CSIM-only path. Record all
results. They become the first modem profile.

### 9.4 First target: Huasifei WH3000 Pro eMMC (answers received 2026-09-17)

| Fact | Consequence |
|---|---|
| Upstream OpenWrt 25.12.5 r33051, `mediatek/filogic`, `aarch64_cortex-a53`, musl 1.2.5, kernel 6.12.94 | Upstream SDK `openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64`; `.apk` packages |
| 2 GB f2fs overlay on eMMC, ~1 GB RAM, no RTC | No size pressure; the NTP gate matters (the clock starts from `sysfixtime`) |
| libcurl 8.19 is built with **mbedTLS 3.6.7**; `libopenssl3` 3.5.7 is installed too | **Blocker for `http.c` as it is** (it needs the OpenSSL flavour). Decision needed, see below |
| `libjansson4`, `jshn`, `jsonfilter`, `libqmi` 1.36, `libmbim` 1.32, `glib2` installed | All runtime dependencies except the TLS question are already on the device |
| **ModemManager 1.24** drives the modem (`network.wan_modem.proto='modemmanager'`); `qmi-proxy` runs | The modem is in QMI mode and shared through `qmi-proxy`. ModemManager also opens AT ports, so an AT transport must not use a port that ModemManager uses |
| **mwan3** with `wan` (Ethernet, DHCP) and `wan_modem` | The eIM may be reached over either WAN. Readiness is "any interface with a default route" by default, or a list of interfaces |
| **rsyslogd** runs, logd does not | syslog still works (`/dev/log`); `logread` does not, so the rpcd `log` method falls back to `/var/log/messages` |
| OpenVPN, zabbix-agentd, uspot installed | Fleet management exists. The status file / `ubus call ipad status` is what a zabbix item can read |

**Decision taken: no LuCI.** The IPAd reads a JSON file (`/etc/ipad/config.json`) that an external tool keeps
up to date, in the Android port's format plus `log.level`, `log.subsys_levels` and a `platform` object. The
daemon starts a cycle by itself when the file changes. `luci-app-ipad` and the UCI configuration were removed
again (§7.2 and §7.6 describe the earlier plan).

**TLS with the stock mbedTLS libcurl: option A chosen and implemented** (see the end of this section).

| Option | Effort | Notes |
|---|---|---|
| A. mbedTLS support in `http.c` (trust anchor via `mbedtls_ssl_config`: `mbedtls_ssl_conf_ca_chain`, and a verify callback for the key-only anchor) | Medium | Uses the device's own libcurl and mbedTLS; no package conflicts. **Recommended.** Testable on the build host only with a self-built curl+mbedTLS |
| B. Replace the device's libcurl with an OpenSSL build | Low code, high operational cost | `libopenssl3` is already there, but the replaced `libcurl4` differs from the official package: `apk upgrade` or a sysupgrade brings the mbedTLS one back |
| C. Link a private static libcurl (OpenSSL) into `ipad` | Low code | Duplicates curl in the image and needs its own security updates |

**Modem findings (2026-09-21).** Two modules were tried with the same probe:

| Modem | Result |
|---|---|
| **Fibocom NL668-EAU** (fw 19305.1000.00.02.73.04) | `AT+CSIM` works, `AT+CCHO`/`AT+CGLA` answer `ERROR`, and SELECT of the ISD-R answers `6999` on every channel, with or without a TERMINAL CAPABILITY first. QMI refuses `--uim-open-logical-channel` with `AccessDenied` and does not implement slot status. **The eUICC cannot be reached through this module.** Its AT manual documents no eSIM or terminal-capability command, and libqmi has no terminal-capability message |
| **Quectel EC200A** (EC200AAUHAR01A11M16) | **Works.** With TERMINAL CAPABILITY first, a card-assigned channel, SELECT of the ISD-R and GET RESPONSE, `AT+CSIM` read the EID of two different eUICCs (Thales `8903302393…`, Linksfield `8904404593…`). The Thales ISD-R FCI reports `ipaeSupported` and `enabledProfile`, and a 255-byte maximum command data field. Not a Qualcomm module: **no QMI** (ECM/RNDIS/PPP), so the WAN side is `ncm`/`ecm` or ModemManager |

So the card was never the problem; the NL668 firmware is. One quirk to carry: on the Linksfield card the GET
RESPONSE that fetches the ISD-R FCI answered `6E00` while every ES10x command worked, so that read is treated
as best-effort.

Consequences:

* **AT+CSIM alone is enough, and it is what was implemented** (§1.2). The core
  already opens the logical channel itself (MANAGE CHANNEL), selects the ISD-R
  and chains GET RESPONSE, exactly as over PC/SC, so the transport is a thin
  wrapper and needs none of the "modem owns the channel" changes of §3.2 items
  1 and 5. What it did need: the channel number chosen by the card, and the CLA
  encoding for channels above 3, since a modem keeps channels of its own (the
  EC200A handed out 1, 2 and 3 in different runs).
* **QMI through `qmi-proxy`** avoids sharing an AT port with ModemManager and
  is the better fit for this router. `qmicli` 1.36 (`apk add qmi-utils`) has
  `--uim-open-logical-channel` / `--uim-send-apdu`, so it can be probed without
  code.

Probe sequences (replace `N` with the channel number returned):

```
# via AT+CSIM -- stop ModemManager first, or use an AT port it does not use (see: mmcli -m any)
AT+CMEE=2                                   verbose errors
AT+CSIM=20,"80AA000005A903840101"           TERMINAL CAPABILITY (IPAd); expect ...9000
AT+CSIM=10,"0070000001"                     MANAGE CHANNEL open; expect "0N9000"
AT+CSIM=42,"0NA4040410A0000005591010FFFFFFFF8900000100"
                                            SELECT ISD-R on channel N; expect 61xx (or FCI + 9000)
AT+CSIM=10,"0NC00000xx"                     GET RESPONSE, xx from the 61xx (the FCI)
AT+CSIM=22,"8NE2910006BF3E035C015A"         GetEID (STORE DATA); expect 61xx
AT+CSIM=10,"0NC00000xx"                     GET RESPONSE: BF3E125A10<EID>9000
AT+CSIM=16,"8NE2910003BF2000"               GetEUICCInfo1, then GET RESPONSE as above
AT+CSIM=8,"0070800N"                        MANAGE CHANNEL close (P2 = N, e.g. 00708001)

# via QMI, alongside ModemManager (slot 1)
qmicli -p -d /dev/cdc-wdm0 --uim-get-card-status
qmicli -p -d /dev/cdc-wdm0 --client-no-release-cid \
       --uim-open-logical-channel="1,A0:00:00:05:59:10:10:FF:FF:FF:FF:89:00:00:01:00"
qmicli -p -d /dev/cdc-wdm0 --client-cid=CID --client-no-release-cid \
       --uim-send-apdu="1,N,80:E2:91:00:06:BF:3E:03:5C:01:5A"   # also try CLA 8N instead of 80
qmicli -p -d /dev/cdc-wdm0 --client-cid=CID --uim-close-logical-channel="1,N"
```

(The `ATI ; AT+CGMM ; …` line in §9.3 lists separate commands, not one command line.)

**Logical channel numbers above 3.** Modems often keep channels 1–3 for their own applications (ISIM, …),
so MANAGE CHANNEL may return 4 or more. ISO/IEC 7816-4 encodes those channels differently in the CLA byte:
`0x40 + (N − 4)` for inter-industry commands (SELECT, GET RESPONSE, MANAGE CHANNEL) and `0xC0 + (N − 4)`
for proprietary ones (STORE DATA); `N` up to 19. Two consequences for the transport work:

* `euicc.c` supports channels 0–3 only (it asserts), ORs the channel into the CLA, and asks MANAGE CHANNEL for
  the fixed `euicc_channel` (P2 = N) rather than letting the card choose one (P2 = 00). With a modem that
  already uses channel 1, both have to change: open with P2 = 00, use the channel the card returns, and
  encode the CLA for channels 4–19.
* When probing by hand, check the channel byte returned before building the SELECT.

**TLS implementation (option A).** `http.c` no longer contains TLS library code. The trust-anchor handling
sits behind `src/ipa/http_tls.h`, implemented by `http_tls_openssl.c` (the previous code, moved) and
`http_tls_mbedtls.c` (new, Mbed TLS 3.x), chosen with `-DIPA_HTTP_TLS=openssl|mbedtls`. The OpenWrt package
follows libcurl's TLS setting. With Mbed TLS:

* `trustedCertificateTls` replaces libcurl's trust store: it is the only trust anchor, where OpenSSL keeps the
  system CAs alongside. A pinned server certificate works even when the server sends a chain.
* `trustedEimPkTls` is accepted on the top certificate of the presented chain, as with OpenSSL.
* The anchors are installed only while verification is on. That also fixes an OpenSSL-side bug: with `-I`
  and a key anchor, the old callback turned peer verification back on.
* Replacing an anchor closes a connection kept open, so the next request is verified against the new one.

`tests/http_tls/` runs 24 handshake cases against local HTTPS servers (anchors of every kind, other roots,
expired and wrong-host certificates, pinned servers, anchor replacement, garbage anchors, `-I`). Results are
identical with OpenSSL (system libcurl) and with Mbed TLS 3.6.7 plus curl 8.19.0 built from source, the
versions on the router. The eIM used so far (`g-eim.com.br`) uses P-256 and a private root delivered as
`trustedCertificateTls`, which the stock OpenWrt Mbed TLS supports. OpenWrt's Mbed TLS disables the
brainpool curves by default, so an eIM with a brainpool certificate would need them enabled.

## 10. What to install on this machine

This box: Debian 12, **1 CPU core, 1.9 GB RAM, ~19 GB free**, no
passwordless sudo, so the `apt` commands must be run by you. Already
present: gcc/g++, make, cmake, git, asn1c, pkg-config, libcurl4-openssl-dev,
libssl-dev, libjansson-dev, libpcsclite-dev, rsync, unzip, wget, zstd, usbutils,
ModemManager tools (`mmcli` 1.20), `qmicli` 1.32, `mbimcli` 1.28, pandoc,
pdflatex, wkhtmltopdf.

| Purpose | Install | Notes |
|---|---|---|
| OpenWrt SDK prerequisites | `sudo apt install bc bison flex gawk file gettext help2man libelf-dev liblzma-dev libncurses-dev python3-dev python3-setuptools swig time xsltproc xxd zlib1g-dev ninja-build ccache` | Subset of the OpenWrt build-system list (which targets Trixie; on Bookworm the same names exist). For a full buildroot add `binutils-gold ecj fastjar libbsd-dev mtd-utils meson mold pbzip2 pigz subversion texinfo u-boot-tools`. |
| OpenWrt SDK | Download from `downloads.openwrt.org/releases/<ver>/targets/<target>/<subtarget>/`, or the vendor's SDK | Unpack in `~/openwrt-sdk/`; roughly 1–2 GB with feeds. Needs the router identified first. |
| Package signing (25.12 apk) | `sdk` generates keys; keep them outside the repo | Needed to install locally built `.apk` without `--allow-untrusted` |
| Cross-test the unit tests | `sudo apt install qemu-user-static binfmt-support` | Run MIPS/ARM test binaries built by the SDK, including big-endian |
| Emulated OpenWrt | `sudo apt install qemu-system-arm qemu-system-mips` (+ `qemu-system-x86` for the x86 image) | Boot `armsr`/`malta`/`x86` images to test procd/UCI/ubus integration without hardware |
| Develop the AT/QMI/MBIM backends natively | `sudo apt install picocom socat libqmi-glib-dev libmbim-glib-dev` | `socat` also helps build a fake modem on a pty for tests |
| Access to modem ttys | `sudo usermod -aG dialout $USER` (then log in again) | Your user is not in `dialout` today |
| ModemManager coexistence | `sudo systemctl stop ModemManager` during AT tests, or a udev rule setting `ID_MM_PORT_IGNORE=1` on the test port | MM grabs every AT port it finds |
| **Hardware on the desk** | A USB modem (or M.2/mPCIe module + USB adapter), ideally the **same module as the router**; the eUICC; a PC/SC reader (already used); a 3.3 V USB-UART adapter for the router console | The AT backend can be written against the desk modem before the router exists |
| Deriving TLS/ESipa test material | already present (`contrib/eim_json2ber.py`, python3) | — |

## 11. Source changes (planned, none made)

| Area | Files | Change |
|---|---|---|
| Transport seam | `include/onomondo/ipa/scard.h`, new `src/ipa/transport/*` | Backend table, URI selection, capability struct; port `ipa_scard_manages_channel` from `ports/android` and extend it |
| Channel handling | `libipa/euicc.c` | Consult capabilities: skip MANAGE CHANNEL/SELECT, use backend channel number, CLA policy, configurable TX/RX block sizes, termcap path, "modem handles proactive" mode, reset through backend. Fix: do not drop data returned with the last STORE DATA's `9000` |
| Config | `include/onomondo/ipa/ipad.h` | `transport` string (keep `reader_num` for compatibility) |
| HTTP | `src/ipa/http.c` | Phase 1: none (OpenSSL). Later: mbedTLS trust-anchor variant |
| Logging | `libipa/log.c`, new sinks | Sink API + file/ring sinks **ported** (§1.1); syslog sink **done** (§1.2) |
| Daemon | `run.c`, `config_json.c`, `fileio.c` (**ported**, §1.1); `src/ipa/ipad_linux.c` (**done**, §1.2) | Loop, preconditions, signals, atomic nvstate, write-on-change, status file for rpcd (§7.6) |
| Build | `CMakeLists.txt` ×3 | Options per backend, no host paths, pre-generated libasn option, `find_package(CURL)` |
| Packaging | new `contrib/openwrt/` (feed: `Makefile`, `files/ipad.init`, `files/ipad.config`, `files/ipad.keep`, `files/hooks.d/`, `files/modems/`) | — |
| Tests | `tests/` | Fake AT modem (pty) covering 61xx vs auto-chain, CLA rewrite, URC interleave, `+CME ERROR`; run under qemu-user |

## 12. Phased plan

| Phase | Content | Exit criterion |
|---|---|---|
| 0. Identify | Router + modem + eUICC facts (§9), probe (§9.3) | Filled checklists; go/no-go on TERMINAL CAPABILITY |
| 1. Build | SDK, feed Makefile, host asn1c or pre-generated codec, PC/SC optional, OpenSSL curl; run existing CLI with PC/SC reader on the router (or in QEMU) | `ipa -h` and unit tests pass for the target arch (qemu-user) |
| 2. AT transport | Backend + capability flags + `euicc.c` changes, developed on this box with a USB modem | GetEID / EUICCInfo1/2 through the modem on the desk, then on the router |
| 3. Daemon | procd service, UCI, syslog sink (file sink already ported), atomic nvstate, preconditions, signals, status file. **Done** except on-device checks (§1.2) | Survives reboot, power cut and WAN loss; logs visible in `logread` |
| 4. End-to-end | Real eIM poll; profile download/enable with WAN re-attach; rollback path; RPLMN; fallback policy | Profile switch completes and the result reaches the eIM over the new profile |
| 5. Generalise | Modem profiles as data, hook scripts, CM adapters (netifd/ModemManager), QMI and/or MBIM backend | A second modem works with only a profile file |
| 6. UI | rpcd exec plugin (`ipad.status/poll/reload/log`), `luci-app-ipad` (status, config, log view); **Done** and tested in an OpenWrt rootfs; LuCI pages still to be checked in a browser | Configurable and observable from LuCI and via `ubus call ipad status` |
| 6b. (later, if needed) | Native ubus object with synchronous policy commands | — |
| 7. Size | mbedTLS variant of `http.c` if flash requires it | Package fits the product's flash budget |

## 13. Risks and open questions

1. **Modem refuses TERMINAL CAPABILITY or raw CSIM.** Then the eUICC may stay
   in "no IPA" or IPAe mode and refuse ES10. Mitigation: QMI/MBIM path,
   vendor command, or a module choice. Test in phase 0.
2. **Modem-internal eSIM/LPA** competing for the ISD-R.
3. **Reset only via `CFUN`**, which makes each profile change a full WAN
   outage. That is acceptable, but timeouts must allow for it.
4. **Vendor firmware** with an old SDK, unavailable GPL tarball or locked
   package installation.
5. **Flash budget** for OpenSSL on small devices (§5).
6. **JSON binding** has not been interop-tested against a real eIM
   (`GETTING_STARTED.md`).
7. **Consumer-eUICC emulation** cannot sign eUICC Package Results (README), so
   it is lab-only.
8. **Open for you to decide:**
   * Which copyright sponsor goes on new OpenWrt-port files? *(To be
     confirmed; no new OpenWrt-specific file has been created yet.)*
   * *Decided:* the Android daemon pieces were ported forward here (§1.1);
     consolidation across ports comes later.
   * *Decided:* ubus and LuCI are wanted in the first delivery. §7.6
     recommends the rpcd-plugin form and postpones the native ubus object.

## Appendix A — References

* GSMA SGP.32 v1.2 (§3.8.4 IPA mode/terminal capability, §5.9 ES10b, §5.14
  ESipa, §6 bindings); GSMA SGP.22 §5.7 (ES10x transport).
* 3GPP TS 27.007 §8.17 (+CSIM), §8.45–8.47 (+CCHO, +CCHC, +CGLA).
* ETSI TS 102 221 (APDU, GET RESPONSE, TERMINAL CAPABILITY §11.1.19),
  ETSI TS 102 223 (proactive REFRESH).
* OpenWrt: build system setup, SDK, `include/cmake.mk`, procd init scripts,
  UCI, hotplug; 25.12 release notes (apk).
* curl: `CURLOPT_SSL_CTX_FUNCTION` (OpenSSL, wolfSSL, mbedTLS only);
  OpenWrt `packages/net/curl/Config.in` (default `LIBCURL_MBEDTLS`).
* lpac (estkme-group) APDU backends `at`, `at_csim`, `qmi`, `qmi_qrtr`,
  `uqmi`, `mbim`, `pcsc`: useful prior art for each transport's quirks.
* Internal: `ports/android` (transport capability, JSON config, log sinks,
  daemon); `ports/windows` (`IPA_LIBASN_GEN_DIR`).
