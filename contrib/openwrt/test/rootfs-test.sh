#!/bin/sh
# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only
#
# Integration test of the OpenWrt packaging (init script, JSON configuration, readiness/event commands, rpcd
# plugin) against a real OpenWrt userland, without root and without the OpenWrt SDK.
#
# It unpacks the official x86-64 rootfs, installs the files of ../ipad into it the way the package Makefile does,
# and runs ubusd, logd, procd (which only manages services when it is not PID 1) and rpcd in an unprivileged
# user/PID/mount namespace chrooted into it. The daemon is the host build of ipad (glibc),
# started through the host's dynamic loader, since the rootfs is musl.
#
# No eUICC is reachable, so every poll cycle fails; what is tested is everything around the cycles.
#
# usage: rootfs-test.sh IPAD_BINARY [WORKDIR]
#   IPAD_BINARY  a host build of ipad (without -DENABLE_SANITIZE, which the rootfs cannot load)
#   WORKDIR      where the rootfs is downloaded and unpacked (default: ./openwrt-rootfs-test)
#
# Needs: unprivileged user namespaces (unshare -r), curl, sha256sum.

set -eu

OPENWRT_VERSION=25.12.5
ROOTFS=openwrt-$OPENWRT_VERSION-x86-64-rootfs.tar.gz
ROOTFS_SHA256=2dae4250b1b742062ed5787adcdc9dd6d26c7f69f94ced46548eb512d5957e8a
ROOTFS_URL=https://downloads.openwrt.org/releases/$OPENWRT_VERSION/targets/x86/64/$ROOTFS

IPAD=$(readlink -f "${1:?usage: $0 IPAD_BINARY [WORKDIR]}")
WORK=$(readlink -m "${2:-./openwrt-rootfs-test}")
PKG=$(dirname "$(readlink -f "$0")")/..
R=$WORK/rootfs

mkdir -p "$WORK"
if [ ! -f "$WORK/$ROOTFS" ]; then
	echo "downloading $ROOTFS_URL"
	curl -fsSL -o "$WORK/$ROOTFS.part" "$ROOTFS_URL"
	mv "$WORK/$ROOTFS.part" "$WORK/$ROOTFS"
fi
echo "$ROOTFS_SHA256  $WORK/$ROOTFS" | sha256sum -c --quiet

# A fresh rootfs every run, so no state leaks from one run into the next.
rm -rf "$R"
mkdir -p "$R"
unshare -r tar -xzf "$WORK/$ROOTFS" -C "$R" 2>/dev/null

# --- install, as the package Makefile does ----------------------------------------------------------------------
install -d "$R/usr/sbin" "$R/etc/init.d" "$R/etc/ipad/hooks.d" "$R/etc/ipad/ready.d" \
	"$R/usr/libexec/ipad" "$R/usr/libexec/rpcd" "$R/etc/hotplug.d/ntp" "$R/lib/upgrade/keep.d"
install -m755 "$PKG/ipad/files/ipad.init" "$R/etc/init.d/ipad"
install -m644 "$PKG/ipad/files/config.json.example" "$R/etc/ipad/config.json.example"
install -m755 "$PKG/ipad/files/ipad.ready" "$R/usr/libexec/ipad/ready"
install -m755 "$PKG/ipad/files/ipad.event" "$R/usr/libexec/ipad/event"
install -m755 "$PKG/ipad/files/ipad.rpcd" "$R/usr/libexec/rpcd/ipad"
install -m644 "$PKG/ipad/files/ipad.ntp-hotplug" "$R/etc/hotplug.d/ntp/25-ipad"
install -m644 "$PKG/ipad/files/ipad.keep" "$R/lib/upgrade/keep.d/ipad"

# The host-built daemon, with the libraries it needs, run through the host loader.
install -d "$R/opt/glibc"
for lib in $(ldd "$IPAD" | awk '{print $3}' | grep /); do
	cp -L "$lib" "$R/opt/glibc/"
done
cp -L /lib64/ld-linux-x86-64.so.2 "$R/opt/glibc/"
install -m755 "$IPAD" "$R/opt/glibc/ipad.bin"
cat > "$R/usr/sbin/ipad" <<'EOF'
#!/bin/sh
exec /opt/glibc/ld-linux-x86-64.so.2 --library-path /opt/glibc /opt/glibc/ipad.bin "$@"
EOF
chmod 755 "$R/usr/sbin/ipad"

# --- namespace setup: device nodes, daemons -------------------------------------------------------------------
cat > "$WORK/enter.sh" <<EOF
#!/bin/sh
for d in null zero urandom random; do
	touch "$R/dev/\$d" 2>/dev/null
	mount --bind "/dev/\$d" "$R/dev/\$d"
done
exec /usr/sbin/chroot "$R" /bin/sh /setup.sh
EOF
chmod +x "$WORK/enter.sh"

cat > "$R/setup.sh" <<'EOF'
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mount -t tmpfs tmpfs /tmp
mount -t proc proc /proc 2>/dev/null
mkdir -p /tmp/run /tmp/lock /tmp/log
/sbin/ubusd >/dev/null 2>&1 &
sleep 1
# logd drops privileges to user "logd", which this namespace cannot map; without the user it keeps running as is.
grep -v '^logd:' /etc/passwd > /tmp/passwd.nologd
mount -o bind /tmp/passwd.nologd /etc/passwd
/sbin/logd -S 64 >/dev/null 2>&1 &
sleep 1
umount /etc/passwd
/sbin/procd -S -d 2 > /tmp/procd.log 2>&1 &
sleep 1
/sbin/rpcd -t 30 > /tmp/rpcd.log 2>&1 &
sleep 1
exec sh /scenario.sh
EOF

# --- the test -------------------------------------------------------------------------------------------------
cat > "$R/scenario.sh" <<'EOF'
failures=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }
check() { name=$1; shift; if eval "$@"; then pass "$name"; else fail "$name"; fi; }
st() { ubus call ipad status | jsonfilter -e "@.$1"; }
# wait up to $2 seconds for condition $1
waitfor() { i=0; while [ $i -lt "$2" ]; do eval "$1" && return 0; sleep 1; i=$((i + 1)); done; return 1; }
CFG=/etc/ipad/config.json
# write the configuration the way a management tool should: complete file, renamed into place
write_config() { cat > "$CFG.new" && mv "$CFG.new" "$CFG"; }

check "rpcd publishes ipad" 'ubus -v list ipad | grep -q "\"poll\""'

/etc/init.d/ipad start
check "no start without a configuration" '! ubus call service list "{\"name\":\"ipad\"}" | grep -q instance1'
check "missing configuration logged" 'logread | grep -q "not starting: /etc/ipad/config.json is missing"'
/usr/sbin/ipad -c /etc/ipad/config.json.example -D /tmp > /tmp/example.out 2>&1 &
example=$!
sleep 2
kill $example
check "example configuration parses" \
	'grep -q "config: loaded /etc/ipad/config.json.example" /tmp/example.out && ! grep -q "rejected" /tmp/example.out'

printf '#!/bin/sh\necho "hook $1 $2" >> /tmp/hook.log\n' > /etc/ipad/hooks.d/10-record
chmod +x /etc/ipad/hooks.d/10-record

write_config <<'JSON'
{
	"reader_num": 99,
	"preferred_eim_id": "eim.example",
	"poll_interval": 3600,
	"nvstate_path": "nvstate.bin",
	"log": { "level": "error", "subsys_levels": { "MAIN": "debug" } },
	"platform": { "require_wan": false, "require_ntp": true, "wan_settle": 2,
		      "restart_on_profile_change": [ "wan_modem" ] }
}
JSON
/etc/init.d/ipad start
check "command line" \
	'ubus call service list "{\"name\":\"ipad\"}" | jsonfilter -e "@.ipad.instances.instance1.command[*]" | tr "\n" " " | grep -q -- "-c /etc/ipad/config.json -D /etc/ipad -S /var/run/ipad/status.json"'
check "NTP gate holds polling" 'waitfor "[ \"\$(st state)\" = waiting ]" 10'
check "status says running" '[ "$(st running)" = true ]'

ACTION=stratum stratum=16 sh -c '. /etc/hotplug.d/ntp/25-ipad'
check "stratum 16 does not count as synchronised" '[ ! -f /var/run/ipad/ntp-synced ]'
ACTION=stratum stratum=2 sh -c '. /etc/hotplug.d/ntp/25-ipad'
check "first cycle after NTP sync" 'waitfor "[ \"\$(st state)\" = sleeping ]" 30'
check "cycle outcome recorded" '[ "$(st cycles)" = 1 ] && [ "$(st last_result)" = error ] && [ -n "$(st next_poll)" ]'
check "event hook called" 'grep -q "^hook cycle-failed -22$" /tmp/hook.log'
check "nvstate relative to /etc/ipad" '[ -s /etc/ipad/nvstate.bin ]'
check "other subsystems at the configured level" '! logread -e "ipad\[" | grep -q "HTTP: HTTP client initialized"'

ubus call ipad poll >/dev/null
check "ubus poll" 'waitfor "[ \"\$(st cycles)\" = 2 ]" 15'
/etc/init.d/ipad poll
check "init script poll" 'waitfor "[ \"\$(st cycles)\" = 3 ]" 15'
check "consecutive failures counted" '[ "$(st consecutive_failures)" = 3 ]'
# MAIN's debug line "nvstate unchanged" appears from the second cycle on.
check "per-subsystem level from the configuration" 'logread -e "ipad\[" | grep -q "daemon.debug ipad\[.*MAIN: "'

pid1=$(st pid)
ubus call ipad reload >/dev/null
check "ubus reload polls" 'waitfor "[ \"\$(st cycles)\" = 4 ]" 15'
/etc/init.d/ipad reload
check "init reload polls" 'waitfor "[ \"\$(st cycles)\" = 5 ]" 15'
check "reload keeps the daemon" '[ "$(st pid)" = "$pid1" ]'

sleep 1
write_config <<'JSON'
{
	"reader_num": 98,
	"poll_interval": 1800,
	"nvstate_path": "nvstate.bin",
	"log": { "level": "debug" },
	"platform": { "require_wan": false, "require_ntp": true, "wan_settle": 2,
		      "restart_on_profile_change": [ "wan_modem" ] }
}
JSON
check "changed configuration polls by itself" 'waitfor "[ \"\$(st cycles)\" = 6 ] && [ \"\$(st state)\" = sleeping ]" 20'
check "new values in effect" '[ "$(st interval)" = 1800 ] && [ "$(st pid)" = "$pid1" ] && logread -e "ipad\[" | grep -q "reader #98"'
check "change logged" 'logread -e "ipad\[" | grep -q "configuration file changed, polling now"'
check "level change from the configuration" 'logread -e "ipad\[" | grep -q "daemon.info ipad\[.*HTTP: HTTP client initialized"'
check "syslog priorities" 'logread -e "ipad\[" | grep -q "daemon.err ipad\[.*MAIN: poll cycle failed"'
check "write-on-change" 'logread -e "ipad\[" | grep -q "nvstate unchanged, not rewriting"'
check "ubus log" '[ "$(ubus call ipad log "{\"lines\":2}" | jsonfilter -e "@.lines[*]" | wc -l)" = 2 ]'

# The transport comes from the configuration and is applied by the front end.
sleep 1
write_config <<'JSON'
{
	"reader_num": 98,
	"poll_interval": 1800,
	"nvstate_path": "nvstate.bin",
	"transport": "at:/dev/null?timeout=300",
	"euicc_channel": "auto",
	"log": { "level": "debug" },
	"platform": { "require_wan": false, "require_ntp": true, "wan_settle": 2,
		      "restart_on_profile_change": [ "wan_modem" ] }
}
JSON
check "transport from the configuration" 'waitfor "logread -e \"ipad\[\" | grep -q \"transport = at:/dev/null\"" 20'
check "AT transport tried and refused" 'logread -e "ipad\[" | grep -q "does not accept AT+CSIM"'
check "auto channel accepted" 'logread -e "ipad\[" | grep -q "euicc_channel = auto"'

ubus listen ipad > /tmp/listen.log 2>&1 &
listener=$!
sleep 1
IPAD_CONFIG=$CFG /usr/libexec/ipad/event profile-changed 2 >/dev/null 2>&1
sleep 1
kill $listener
check "event published on ubus" 'grep -q "\"profile-changed\"" /tmp/listen.log'
check "profile change recorded" '[ -s /var/run/ipad/profile-changed ]'
check "interfaces restarted on profile change" 'logread | grep -q "profile changed, restarting interface wan_modem"'
check "hooks get profile-changed" 'grep -q "^hook profile-changed 2$" /tmp/hook.log'

ready() { IPAD_CONFIG=$CFG /usr/libexec/ipad/ready "$@"; }
check "not ready within settle time" '! ready profile-changed'
sleep 3
check "ready after settle time (WAN not required)" 'ready profile-changed'
write_config <<'JSON'
{ "reader_num": 98, "poll_interval": 1800, "platform": { "wan_interfaces": [ "wan", "wan_modem" ] } }
JSON
check "not ready: listed WANs unknown to netifd" '! ready poll'
write_config <<'JSON'
{ "reader_num": 98, "poll_interval": 1800, "platform": { "require_ntp": false } }
JSON
check "not ready: no default route" '! ready poll'
# The default-route query, against a netifd dump of a dual-WAN router: modem up, Ethernet down.
dump='{"interface":[{"interface":"wan","up":false,"route":[]},{"interface":"wan_modem","up":true,"route":[{"target":"0.0.0.0","mask":0}]},{"interface":"lan","up":true,"route":[]}]}'
check "default-route query finds an up interface" \
	'jsonfilter -q -s "$dump" -e "@.interface[@.up=true].route[@.target=\"0.0.0.0\"]" | grep -q .'
dump_down='{"interface":[{"interface":"wan_modem","up":false,"route":[{"target":"0.0.0.0","mask":0}]},{"interface":"lan","up":true,"route":[]}]}'
check "default-route query ignores down interfaces" \
	'! jsonfilter -q -s "$dump_down" -e "@.interface[@.up=true].route[@.target=\"0.0.0.0\"]" | grep -q .'
write_config <<'JSON'
{ "reader_num": 98, "poll_interval": 1800, "platform": { "require_wan": false, "require_ntp": false } }
JSON
check "ready when nothing is required" 'ready poll'
printf '#!/bin/sh\nexit 3\n' > /etc/ipad/ready.d/50-no; chmod +x /etc/ipad/ready.d/50-no
check "ready.d can veto" '! ready poll'
rm /etc/ipad/ready.d/50-no

/etc/init.d/ipad stop
check "stop" 'waitfor "[ \"\$(jsonfilter -i /var/run/ipad/status.json -e @.state)\" = stopped ]" 40'
check "status after stop" '[ "$(st running)" = false ]'
check "poll refused when stopped" 'ubus call ipad poll | jsonfilter -e @.result | grep -qx error'
check "clean exit" 'grep -q "ipad::instance1 exit with error code 0" /tmp/procd.log'

# Systems running rsyslog instead of logd: the log comes from rsyslog's file.
killall logd
sleep 1
echo "Sep 17 12:00:00 router daemon.info ipad[42]: MAIN: from rsyslog" > /var/log/messages
check "ubus log without logd" 'ubus call ipad log | jsonfilter -e "@.lines[*]" | grep -q "from rsyslog"'

echo "$failures failure(s)"
[ "$failures" -eq 0 ]
EOF

timeout 600 unshare -r -p -f -m --mount-proc "$WORK/enter.sh"
