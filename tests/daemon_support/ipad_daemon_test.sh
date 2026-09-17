#!/bin/sh
# Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
# SPDX-License-Identifier: AGPL-3.0-only
#
# Drives the ipad binary as a supervisor would. No eUICC is reachable, so every cycle fails; what is checked is the
# daemon around the cycles: single-cycle mode, status file, event command, readiness command, and signals.

set -u
IPAD=$1
T=$(mktemp -d /tmp/ipad_daemon_test_XXXXXX)
trap 'kill $PID 2>/dev/null; rm -rf "$T"' EXIT
PID=

fail() { echo "FAIL: $*"; exit 1; }

# The status file must be valid JSON; python3 is used to check it when present.
field() {
	python3 -c 'import json,sys; v=json.load(open(sys.argv[1]))[sys.argv[2]]; print("null" if v is None else v)' "$1" "$2"
}

# Wait up to $2 tenths of a second for a condition.
wait_for() {
	i=0
	while [ $i -lt "$2" ]; do
		eval "$1" && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

# A reader number nobody has, so the eUICC is unreachable whether or not pcscd runs here.
cat > "$T/config.json" <<JSON
{ "reader_num": 99, "nvstate_path": "$T/nvstate.bin" }
JSON

cat > "$T/event.sh" <<'SH'
#!/bin/sh
echo "$1 $2" >> "$(dirname "$0")/events"
SH
chmod +x "$T/event.sh"

echo "single_cycle_test"
"$IPAD" -c "$T/config.json" -S "$T/status.json" -e "$T/event.sh" > "$T/out1" 2>&1
[ $? -ne 0 ] || fail "single failed cycle must exit non-zero"
[ "$(field "$T/status.json" state)" = stopped ] || fail "state after single cycle"
[ "$(field "$T/status.json" cycles)" = 1 ] || fail "cycles after single cycle"
[ "$(field "$T/status.json" last_result)" = error ] || fail "last_result"
grep -q '^cycle-failed -' "$T/events" || fail "cycle-failed event missing"
[ -f "$T/nvstate.bin" ] || fail "nvstate not saved"

echo "bad_config_test"
"$IPAD" -c "$T/missing.json" > "$T/out2" 2>&1 && fail "missing configuration must fail"

echo "readiness_test"
# Not ready on the first call, ready afterwards: the daemon must wait, then poll.
cat > "$T/ready.sh" <<'SH'
#!/bin/sh
d=$(dirname "$0")
echo "$1" >> "$d/ready_calls"
env | grep '^IPAD_' >> "$d/env"
[ -f "$d/ready_ok" ]
SH
chmod +x "$T/ready.sh"
rm -f "$T/events"
"$IPAD" -c "$T/config.json" -S "$T/status.json" -e "$T/event.sh" -w "$T/ready.sh" -i 3600 > "$T/out3" 2>&1 &
PID=$!
wait_for '[ -f "$T/status.json" ] && [ "$(field "$T/status.json" state)" = waiting ]' 50 || fail "not waiting"
[ ! -f "$T/events" ] || fail "polled while not ready"
touch "$T/ready_ok"
kill -USR2 $PID
wait_for '[ "$(field "$T/status.json" state)" = sleeping ]' 50 || fail "did not poll once ready"
grep -q '^poll$' "$T/ready_calls" || fail "readiness command not given its reason"
[ "$(field "$T/status.json" cycles)" = 1 ] || fail "cycles after readiness"
[ "$(field "$T/status.json" next_poll)" != null ] || fail "next_poll missing while sleeping"

echo "poll_now_test"
kill -USR2 $PID
wait_for '[ "$(field "$T/status.json" cycles)" = 2 ]' 50 || fail "SIGUSR2 did not trigger a cycle"
[ "$(field "$T/status.json" consecutive_failures)" = 2 ] || fail "consecutive_failures"

echo "reload_test"
kill -HUP $PID
wait_for '[ "$(field "$T/status.json" cycles)" = 3 ]' 50 || fail "SIGHUP did not trigger a cycle"
grep -q 'reload requested' "$T/out3" || fail "reload not logged"

echo "config_change_test"
c=$(field "$T/status.json" cycles)
sleep 1
# A new file renamed into place, as a configuration tool would do it.
cat > "$T/config.json.new" <<JSON
{ "reader_num": 98, "nvstate_path": "$T/nvstate.bin", "log": { "level": "error", "subsys_levels": { "MAIN": "info" } },
  "platform": { "wan_interfaces": [ "wan_modem" ] } }
JSON
mv "$T/config.json.new" "$T/config.json"
wait_for '[ "$(field "$T/status.json" cycles)" = $((c + 1)) ]' 100 || fail "a changed configuration did not start a cycle"
grep -q 'configuration file changed' "$T/out3" || fail "configuration change not logged"
wait_for '[ "$(field "$T/status.json" state)" = sleeping ]' 50 || fail "not sleeping after the change"
# log.level=error silences every other subsystem's info lines; subsys_levels keeps MAIN at info, not debug.
# (Which subsystems log at all depends on the build, so only MAIN's lines are relied upon.)
sed -n '/configuration file changed/,$p' "$T/out3" > "$T/cycle4"
grep -q 'MAIN     INFO IPAd starting' "$T/cycle4" || fail "MAIN info missing with subsys_levels MAIN=info"
! grep -q 'MAIN    DEBUG' "$T/cycle4" || fail "MAIN debug logged although its level is info"
! grep -Eq '^ *(HTTP|SCARD|IPA|ES10x|ES10b|eUICC|ESIPA) +INFO' "$T/cycle4" || fail "log.level=error not applied"
grep -q 'IPAD_CONFIG=/' "$T/env" || fail "IPAD_CONFIG not passed to the readiness command"

echo "level_fallback_test"
c=$(field "$T/status.json" cycles)
cat > "$T/config.json.new" <<JSON
{ "reader_num": 97, "nvstate_path": "$T/nvstate.bin" }
JSON
mv "$T/config.json.new" "$T/config.json"
wait_for '[ "$(field "$T/status.json" cycles)" = $((c + 1)) ] && [ "$(field "$T/status.json" state)" = sleeping ]' 100 ||
	fail "second change did not start a cycle"
# No -l on this daemon's command line, so the fallback is the library default: everything, MAIN's debug included.
sed -n '/reader_num = 97/,$p' "$T/out3" | grep -q 'MAIN    DEBUG' || fail "levels removed from the file did not fall back"

echo "stop_test"
kill -TERM $PID
wait_for '! kill -0 $PID 2>/dev/null' 50 || fail "SIGTERM did not stop the daemon"
wait $PID
[ $? -eq 0 ] || fail "supervised daemon must exit 0 on stop"
PID=
[ "$(field "$T/status.json" state)" = stopped ] || fail "state after stop"
[ ! -f "$T/status.json.tmp" ] || fail "temporary status file left behind"

echo "ipad_daemon_test: all tests passed"
