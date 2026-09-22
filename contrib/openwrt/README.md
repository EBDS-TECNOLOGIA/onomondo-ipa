# OpenWrt feed for the IPAd

One package, `ipad`: the daemon `/usr/sbin/ipad`, the CLI `/usr/bin/ipa`, the procd init script, the readiness
and event commands, and the rpcd plugin that publishes ubus object `ipad`. There is no LuCI app: the IPAd is
configured by a JSON file that another program maintains.

Background and design: [`OPENWRT_PORT_ANALYSIS.md`](../../OPENWRT_PORT_ANALYSIS.md), sections 7 and 7.6.

**State:** the eUICC is reached either through a modem's `AT+CSIM` or through a PC/SC reader, see *Reaching the
eUICC* below. QMI and MBIM transports are not implemented.

## Building

Use the OpenWrt SDK that matches the router's firmware exactly (release, target, subtarget), or the vendor's
SDK for a vendor fork.

1. **Host:** besides the SDK prerequisites, install `asn1c` (`apt install asn1c`). The ASN.1 codec is generated
   on the build host while the package is configured, and the SDK does not provide asn1c.
2. **Feed:** add this directory as a feed. In `feeds.conf` of the SDK:
   ```
   src-link ipad /path/to/onomondo-ipa/contrib/openwrt
   ```
   then:
   ```
   ./scripts/feeds update ipad
   ./scripts/feeds install -p ipad ipad
   ```
3. **TLS library.** `http.c` installs the eIM trust anchor through the TLS library libcurl is built with. The
   package follows libcurl's setting (*Libraries → libcurl → SSL library*): Mbed TLS, OpenWrt's default and what
   stock images ship, or OpenSSL. Other choices hide the package. Outside OpenWrt the choice is
   `-DIPA_HTTP_TLS=openssl|mbedtls` (default `openssl`).
4. **Source.** The Makefile fetches `PKG_SOURCE_VERSION` from GitHub; pin it to a commit for a release. To
   build a local working tree instead:
   ```
   ln -s /path/to/onomondo-ipa/.git package/feeds/ipad/ipad/git-src
   make menuconfig    # Advanced configuration options → Enable package source tree override
   ```
5. **Build:**
   ```
   make package/ipad/compile V=s
   ```
   The packages end up under `bin/packages/<arch>/ipad/`. They are `.apk` from OpenWrt 25.12 on, and `.ipk`
   before that.

## Configuration

`/etc/ipad/config.json`, in the format shared with the other ports; `/etc/ipad/config.json.example` documents
it. The service does not start without it. The daemon re-reads the file on every poll cycle and starts a cycle
by itself when the file changes, so the program that maintains it needs to do nothing else. It should write a
complete new file and rename it into place, so the daemon never reads half a file.

Relative paths in the file (such as the default `nvstate_path`) are relative to `/etc/ipad`, which is kept
across sysupgrade.

Additions to the format of the Android port:

| Key | Meaning |
|---|---|
| `log.level` | `error`, `info` or `debug` for every subsystem; without it the service logs at `info` |
| `log.subsys_levels` | per subsystem, e.g. `{"ES10x": "debug"}`; subsystems: MAIN, HTTP, SCARD, IPA, ES10x, ES10b, eUICC, ESIPA |
| `transport` | how the eUICC is reached (see below); absent means PC/SC with `reader_num` |
| `euicc_channel` | `"auto"` (the eUICC picks the logical channel, needed with a modem), a number, or 0 for the basic channel |
| `platform` | settings of the OpenWrt scripts below; the IPAd itself only checks that it is an object |

`platform` keys (all optional):

| Key | Default | Meaning |
|---|---|---|
| `require_wan` | `true` | wait for the WAN before polling |
| `wan_interfaces` | `[]` | netifd interfaces, of which one must be up (several with mwan3); empty: any interface that is up and provides a default route |
| `require_ntp` | `true` | wait until sysntpd has synchronised the clock (ignored when the NTP client is disabled) |
| `wan_settle` | `30` | seconds after a profile change before the WAN is checked again |
| `restart_on_profile_change` | `[]` | interfaces to `ifup` after a profile change |

The initial eIM configuration of an unprovisioned eUICC is loaded with the CLI while the daemon is stopped:

```
/etc/init.d/ipad stop
ipa -r 0 -n /etc/ipad/nvstate.bin -f /path/to/AddInitialEimRequest.ber
/etc/init.d/ipad start
```

## Reaching the eUICC

`transport` in the configuration selects it:

| URI | Meaning |
|---|---|
| `at:/dev/ttyUSB2` | APDUs through the modem's `AT+CSIM` (3GPP TS 27.007 §8.17) |
| `pcsc:0` | PC/SC reader 0, for a USB card reader |

Options are appended to the AT URI as `?name=value&name=value`:

| Option | Default | Meaning |
|---|---|---|
| `timeout` | 5000 | milliseconds to wait for the answer to one AT command |
| `baud` | unchanged | line speed; USB ports ignore it |
| `reset` | `none` | how to reset the card, which an eUICC asks for after a profile change: `none` leaves it to the platform, `cfun` sends `AT+CFUN=0` then `AT+CFUN=1` (this also drops the radio) |
| `resetwait` | 20000 | milliseconds to wait for the card after a reset |
| `quirks=echo` | off | leave the modem's command echo on instead of sending `ATE0` |

Set `"euicc_channel": "auto"` with a modem: it keeps logical channels of its own, so the eUICC has to pick a
free one. Channels 4 to 19 are addressed correctly.

**ModemManager, or anything else using the port.** The IPAd locks its AT port with `flock(2)`, which keeps two
IPAd instances apart but says nothing to ModemManager, which takes no lock. Either give the IPAd a port
ModemManager does not use — `mmcli -m any` lists the ones it does — or stop it while testing:

```
/etc/init.d/modemmanager stop
```

Two modems seen so far: a **Quectel EC200A** reads the EID of two different eUICCs over `AT+CSIM`, while a
**Fibocom NL668** refuses to select the ISD-R (`6999`, and `AccessDenied` over QMI), so the eUICC cannot be
reached through it at all. The IPAd does not use `AT+CCHO`/`AT+CGLA`: the core opens the channel and chains
`GET RESPONSE` itself, exactly as it does over PC/SC.

The CLI takes the same URI with `-T`, which is how the initial eIM configuration is loaded through a modem:

```
/etc/init.d/ipad stop
ipa -T at:/dev/ttyUSB2 -c auto -n /etc/ipad/nvstate.bin -f /path/to/AddInitialEimRequest.ber
/etc/init.d/ipad start
```

## Operation

| | |
|---|---|
| Log | syslog: `logread -e ipad` with logd, or rsyslog's files; optionally also a rotating file (`log.path`) |
| Status | `ubus call ipad status`, backed by `/var/run/ipad/status.json` |
| Poll now | `ubus call ipad poll` or `/etc/init.d/ipad poll` |
| Events | `ubus listen ipad` |
| State on flash | `/etc/ipad/nvstate.bin`, kept across sysupgrade, rewritten only when it changes, replaced atomically |

ubus methods (rpcd plugin `/usr/libexec/rpcd/ipad`):

| Method | Arguments | Result |
|---|---|---|
| `status` | – | the status file plus `running` |
| `poll` | – | `{"result":"ok"}` or an error when the service is not running |
| `reload` | – | re-reads the configuration and polls now (also `/etc/init.d/ipad reload`) |
| `log` | `lines` (default 100, max 1000) | `{"lines":[...]}` from logd, or from `/var/log/messages` when rsyslog runs instead |

Status fields: `state` (`starting`, `waiting`, `polling`, `sleeping`, `stopped`), `pid`, `version`, `interval`,
`started`, `updated`, `cycles`, `consecutive_failures`, `last_start`, `last_end`, `last_rc`, `last_result`
(`ok`/`error`), `last_poll_rc`, `last_poll_result` (`done`, `again`, `profile_changed`, `euicc_unreachable`,
`eim_unreachable`, `error`), `next_poll`, `eid`, `eim_id`, `eim_fqdn`, `ipa_mode` (`ipad`, `ipae`, `unknown`).
Times are Unix seconds; values not known yet are `null`.

Daemon signals: `TERM`/`INT`/`USR1` stop, `USR2` polls now, `HUP` re-reads the configuration and polls now.

## Adapting to a board

Nothing here needs C changes:

* **`/etc/ipad/ready.d/*`**: extra readiness conditions. Each executable is called with the reason (`poll` or
  `profile-changed`), and polling waits until all of them exit 0. The built-in checks are the WAN, the NTP clock
  (released by `/etc/hotplug.d/ntp/25-ipad`) and a settle time after a profile change, see `platform` above.
  `IPAD_CONFIG` in the environment points at the configuration, so a script can read its own `platform` keys
  with `jsonfilter -i "$IPAD_CONFIG" -e ...`.
* **`/etc/ipad/hooks.d/*`**: reactions to events, called as `HOOK EVENT RC` with `cycle-done`, `cycle-failed`
  or `profile-changed`. A hook that switches the APN after a profile change goes here.

## Testing without a router

`test/rootfs-test.sh` runs the packaging against the official OpenWrt x86-64 rootfs in an unprivileged
namespace, with procd, ubus, rpcd and logd. It needs no root and no SDK, only a host build of `ipad` without
AddressSanitizer:

```
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel --target ipad
contrib/openwrt/test/rootfs-test.sh build-rel/src/ipa/ipad /tmp/ipad-rootfs-test
```
