#!/usr/bin/env bash
# Tests tools/kindle-probe.sh: the touch-panel capability reader against the
# values real devices report, and a whole run on this machine (which is not a
# Kindle, so it must still finish, say what it could not find, and print a
# usable report).
#
#   bash scripts/test_probe.sh
set -u
cd "$(dirname "$0")/.."
PROBE="$PWD/tools/kindle-probe.sh"

pass=0; fail=0
check() { local d="$1"; shift; if "$@"; then pass=$((pass+1)); echo "  ok   $d"; else fail=$((fail+1)); echo "  FAIL $d"; fi; }
proto() { PROBE_LIB=1 sh -c ". '$PROBE'; touch_protocol \"\$1\" \"\$2\"" _ "$1" "$2"; }
has() { PROBE_LIB=1 sh -c ". '$PROBE'; abs_has \"\$1\" \"\$2\" \"\$3\"" _ "$1" "$2" "$3"; }

echo "touch panel capabilities"
# Real values: the 8th-generation Kindle's zforce2 panel (type-B multitouch).
check "the Kindle 8th gen panel is type-B multitouch" test "$(proto '2608000 0' 32)" = mt-slot
# Bits 53 and 54 (MT position) without 47 (MT slot): type A.
check "position without slots is type-A multitouch" test "$(proto '600000 0' 32)" = mt-type-a
# Bits 0 and 1 (X, Y): an old single-touch panel.
check "X and Y only is single-touch" test "$(proto '0 3' 32)" = single-touch
check "nothing is none" test "$(proto '0 0' 32)" = none
# The same panel on a 64-bit kernel prints one 64-bit word.
w64="$(printf '%x' $(( (1<<47) | (1<<53) | (1<<54) | (1<<57) )))"
check "64-bit words are read too" test "$(proto "$w64" 64)" = mt-slot
check "a lone word that is not set" test "$(proto '0' 64)" = none

echo "single bits"
check "bit 47 of '2608000 0' is set" has '2608000 0' 32 47
check "bit 57 (tracking id) is set" has '2608000 0' 32 57
check "bit 46 is clear" sh -c "PROBE_LIB=1; . '$PROBE'; ! abs_has '2608000 0' 32 46"
check "a bit beyond the words is clear" sh -c "PROBE_LIB=1; . '$PROBE'; ! abs_has '2608000 0' 32 200"
check "bit 0 of the low word" sh -c "PROBE_LIB=1; . '$PROBE'; abs_has '2608000 1' 32 0"
check "bit 31 of the low word" sh -c "PROBE_LIB=1; . '$PROBE'; abs_has '2608000 80000000' 32 31"

echo "a whole run on a machine that is not a Kindle"
REPORT="$(mktemp)"; trap 'rm -f "$REPORT" "$CODE"' EXIT
sh "$PROBE" > "$REPORT" 2>&1; rc=$?
check "finishes cleanly" test "$rc" = 0
check "ends with its done marker" grep -q '^== done ==' "$REPORT"
check "is wrapped in begin and end markers to copy between" sh -c "head -1 '$REPORT' | grep -q 'BEGIN KINDLE REPORT' && grep -q '^===== END KINDLE REPORT' '$REPORT'"
check "has every section" sh -c "for s in software cpu screen touch 'front light' 'battery and charger' 'stock ui' 'cron and ssh' 'tools and network' storage 'suggested platform'; do grep -q \"^== \$s ==\" '$REPORT' || exit 1; done"
check "suggests a build.env" grep -q 'GOOS=linux' "$REPORT"
check "suggests a profile.json with a touch section" sh -c "sed -n '/^profile.json:/,/^}/p' '$REPORT' | grep -q '\"touch\"'"
check "says when it found no front light" grep -q 'verdict: no front light found' "$REPORT"
check "prints no error messages from missing tools" sh -c "! grep -qiE 'syntax error|permission denied|command not found|: not found' '$REPORT'"

echo "it only reads"
# The script's code without comments, without the lines that only print or only
# list tool names, and without text in quotes: what is left is the commands it runs.
CODE="$(mktemp)"
grep -vE '^[[:space:]]*(#|echo|printf|for t in)' "$PROBE" | sed 's/"[^"]*"//g' > "$CODE"
check "calls nothing that changes the device" sh -c "! grep -wE 'rm|mv|cp|chmod|chown|mntroot|reboot|kill|killall|tee|start|stop|lipc-set-prop|mkdir|touch|dd|sed -i' '$CODE'"
check "never redirects output anywhere but the void" sh -c "! sed 's/2>&1//g; s/2> *\/dev\/null//g; s/> *\/dev\/null//g; /v >> (/d' '$CODE' | grep -E '>'"

echo "privacy"
check "does not read the MAC address, IP address or WiFi names" sh -c "! grep -qE 'ifconfig|wlan0|HWaddr|/address|iwconfig|wpa_cli|ssid|SSID|wpa_supplicant.conf' '$PROBE'"
check "prints only four characters of the serial" sh -c "grep -q 'head -c 4 /proc/usid' '$PROBE' && ! grep '/proc/usid' '$PROBE' | grep -qv 'head -c 4'"

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
