#!/usr/bin/env bash
# Exercises platforms/kindle-pw3/install/startpaper.sh against stand-ins for
# the Kindle's commands (initctl, lipc, pidof, the app itself), so the crash
# handling, rollback and off switch can be checked without a device.
#
#   bash scripts/test_launcher.sh
set -u
cd "$(dirname "$0")/.."
LAUNCHER="$PWD/platforms/kindle-pw3/install/startpaper.sh"

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
export T
mkdir -p "$T/bin" "$T/dir" "$T/jobs"
export DIR="$T/dir" STATE="$T/state"

# --- stand-ins ----------------------------------------------------------------
cat > "$T/bin/initctl" <<'EOF'
#!/bin/sh
# initctl status JOB
if [ -f "$T/jobs/$2" ]; then echo "$2 start/running"; else echo "$2 stop/waiting"; fi
EOF
cat > "$T/bin/stop" <<'EOF'
#!/bin/sh
rm -f "$T/jobs/$1"; echo "stop $1" >> "$T/calls"
EOF
cat > "$T/bin/start" <<'EOF'
#!/bin/sh
touch "$T/jobs/$1"; echo "start $1" >> "$T/calls"
EOF
cat > "$T/bin/lipc-set-prop" <<'EOF'
#!/bin/sh
echo "lipc $*" >> "$T/calls"
EOF
# pid is deliberately impossible, so the script's `kill` can't hit a real process.
cat > "$T/bin/pidof" <<'EOF'
#!/bin/sh
[ -f "$T/running" ] && { echo 2147483000; exit 0; }
exit 1
EOF
cat > "$DIR/paperdisplay" <<'EOF'
#!/bin/sh
echo "$@" >> "$T/started"
exit "${STUB_EXIT:-0}"
EOF
chmod +x "$T"/bin/* "$DIR/paperdisplay"

pass=0; fail=0
check() { # check "description" command...
  local desc="$1"; shift
  if "$@"; then pass=$((pass+1)); echo "  ok   $desc"; else fail=$((fail+1)); echo "  FAIL $desc"; fi
}
launch() { PATH="$T/bin:$PATH" sh "$LAUNCHER"; }
# The app runs in a background wrapper that records its exit status.
wait_exit() { for _ in $(seq 1 50); do [ -f "$STATE/exit" ] && return; sleep 0.1; done; }
starts() { [ -f "$T/started" ] && wc -l < "$T/started" || echo 0; }
called() { grep -qx "$1" "$T/calls" 2>/dev/null; }
logged() { grep -q "$1" "$DIR/paperdisplay.log" 2>/dev/null; }
reset() { rm -rf "$T/started" "$T/calls" "$T/running" "$STATE" "$DIR/paperdisplay.log" "$DIR/disable" \
          "$DIR/paperdisplay.prev" "$DIR/paperdisplay.bad"; rm -f "$T/jobs"/*; touch "$T/jobs/framework"; }

echo "no config: refuses to start"
reset
launch
check "app not started" test "$(starts)" = 0
check "explains what is missing" logged "SIGNALK_HOST is not set"

echo "normal start"
reset; echo 'SIGNALK_HOST=10.0.0.76:3001' > "$DIR/launcher.conf"
launch; wait_exit
check "app started once" test "$(starts)" = 1
check "passes the server and touch/fbink flags" grep -q -- '-signalk 10.0.0.76:3001 -display fbink -touch' "$T/started"
check "stops the stock framework" called "stop framework"
check "hides the stock status bar" called "lipc com.lab126.pillow disableEnablePillow disable"
check "keeps the screen awake" called "lipc com.lab126.powerd preventScreenSaver 1"

echo "already running: never starts a second copy"
reset; echo 'SIGNALK_HOST=h:1' > "$DIR/launcher.conf"; touch "$T/running"
launch
check "nothing started" test "$(starts)" = 0
check "still keeps the screen awake" called "lipc com.lab126.powerd preventScreenSaver 1"

echo "clean exit (e.g. after an update) is not a crash"
reset; echo 'SIGNALK_HOST=h:1' > "$DIR/launcher.conf"
for _ in 1 2 3 4 5 6; do STUB_EXIT=0 launch; wait_exit; done
check "restarted every time" test "$(starts)" = 6
check "no crash recorded" test ! -f "$STATE/crashes"
check "stock UI left alone" test ! "$(grep -c 'start framework' "$T/calls" 2>/dev/null || true)" -gt 0

echo "crash loop: roll back, then give up and restore the stock UI"
reset; echo 'SIGNALK_HOST=h:1' > "$DIR/launcher.conf"
# The previous version is another stub app that also crashes, so the
# rolled-back copy keeps failing and the give-up path is reached.
printf '#!/bin/sh\n# previous build\nexit "${STUB_EXIT:-0}"\n' > "$DIR/paperdisplay.prev"
chmod +x "$DIR/paperdisplay.prev"
export STUB_EXIT=1
for _ in 1 2 3 4; do launch; wait_exit; done
check "rolled back to the previous version after 3 crashes" logged "rolling back"
check "bad binary kept aside" test -f "$DIR/paperdisplay.bad"
check "previous version is now the app" grep -q "previous build" "$DIR/paperdisplay"
# the rolled-back copy crashes too, so more failures follow
for _ in 1 2 3 4 5 6 7; do launch; wait_exit; done
check "gives up eventually" logged "restoring the stock Kindle UI"
check "brings the stock framework back" called "start framework"
unset STUB_EXIT

echo "disable file: stops the app and restores the stock UI"
reset; echo 'SIGNALK_HOST=h:1' > "$DIR/launcher.conf"; touch "$T/running" "$DIR/disable"; rm -f "$T/jobs/framework"
launch
check "app not started" test "$(starts)" = 0
check "says it is stopping the app" logged "disable file present"
check "stock framework started again" called "start framework"
check "stock status bar re-enabled" called "lipc com.lab126.pillow disableEnablePillow enable"

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
