#!/usr/bin/env bash
# Exercises platforms/kindle-pw3/install/install-cron.sh against a stand-in
# crontab and a stand-in mntroot, so the editing, the backup, the read-only
# window and the failure paths are checked without a device.
#
#   bash scripts/test_install_cron.sh
set -u
cd "$(dirname "$0")/.."
SCRIPT="$PWD/platforms/kindle-pw3/install/install-cron.sh"

T="$(mktemp -d)"
trap 'kill $LIVE 2>/dev/null; chmod -R u+w "$T" 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/cron" "$T/app" "$T/live"
export DIR="$T/app" CRON_DIR="$T/cron" SSH_PIDFILE="$T/dropbear.pid"
export MNTROOT="$T/bin/mntroot"
LAUNCHER="$DIR/startpaper.sh"
ENTRY="* * * * * $LAUNCHER > /dev/null 2>&1"

# mntroot records what it was asked; MNTROOT_FAIL=rw makes that call fail.
cat > "$T/bin/mntroot" <<'EOF'
#!/bin/sh
echo "mntroot $1" >> "$T/calls"
[ "${MNTROOT_FAIL:-}" = "$1" ] && exit 1
exit 0
EOF
chmod +x "$T/bin/mntroot"
export T

# A live dropbear: a process whose /proc comm is "dropbear".
cp "$(command -v sleep)" "$T/live/dropbear"
"$T/live/dropbear" 600 & LIVE=$!
echo "$LIVE" > "$SSH_PIDFILE"

ORIGINAL='*/15 * * * * /usr/sbin/tinyrot
*/30 * * * * initctl emit tmpfs_scan
0 0 * * * /etc/dem_heartbeat/record_dem_heartbeat_cronjob.sh
* * * * * /usr/bin/startssh.sh > /dev/null 2>&1'

pass=0; fail=0
check() { local d="$1"; shift; if "$@"; then pass=$((pass+1)); echo "  ok   $d"; else fail=$((fail+1)); echo "  FAIL $d"; fi; }
fresh() { printf '%s\n' "$ORIGINAL" > "$CRON_DIR/root"; rm -f "$T/calls" "$DIR/crontab.root.bak"; unset MNTROOT_FAIL; chmod u+w "$CRON_DIR/root"; }
run() { sh "$SCRIPT" "$@" > "$T/out" 2>&1; echo $? > "$T/rc"; }
rc() { [ "$(cat "$T/rc")" = "$1" ]; }
calls() { cat "$T/calls" 2>/dev/null | tr '\n' ' '; }
count() { grep -cF -- "$1" "$CRON_DIR/root"; }

echo "status"
fresh; run status
check "reports the launcher absent and startssh present" sh -c "grep -q 'launcher cron entry: absent' $T/out && grep -q 'startssh.sh cron entry: present' $T/out"
check "changes nothing" test "$(cat "$CRON_DIR/root")" = "$ORIGINAL"
check "never touches mntroot" test ! -e "$T/calls"

echo "add"
fresh; run add
check "succeeds" rc 0
check "adds the launcher line exactly" test "$(count "$ENTRY")" = 1
check "keeps every other line" sh -c "printf '%s\n' \"$ORIGINAL\" | while read -r l; do grep -qF -- \"\$l\" $CRON_DIR/root || exit 1; done"
check "leaves the startssh line without the flag" test "$(count startssh.sh)" = 1
check "backs the table up first, unchanged" test "$(cat "$DIR/crontab.root.bak")" = "$ORIGINAL"
check "makes the filesystem writable and then read-only, in that order" test "$(calls)" = "mntroot rw mntroot ro "
run status
check "status then says present" grep -q 'launcher cron entry: present' "$T/out"

echo "add again"
rm -f "$T/calls"; run add
check "succeeds" rc 0
check "adds nothing a second time" test "$(count "$ENTRY")" = 1
check "does not even open the root filesystem" test ! -e "$T/calls"

echo "add with --remove-startssh"
fresh; run add --remove-startssh
check "succeeds" rc 0
check "adds the launcher" test "$(count "$ENTRY")" = 1
check "removes the startssh line" test "$(count startssh.sh)" = 0
check "keeps the unrelated lines" sh -c "grep -qF tinyrot $CRON_DIR/root && grep -qF tmpfs_scan $CRON_DIR/root && grep -qF dem_heartbeat $CRON_DIR/root"
check "one writable window for both edits" test "$(calls)" = "mntroot rw mntroot ro "

echo "will not remove startssh with no live dropbear"
fresh; kill "$LIVE"; sleep 0.2
run add --remove-startssh
check "refuses" rc 1
check "says why" grep -q 'no live dropbear' "$T/out"
check "changes nothing at all" test "$(cat "$CRON_DIR/root")" = "$ORIGINAL"
check "never opens the root filesystem" test ! -e "$T/calls"
run remove-startssh
check "remove-startssh refuses too" rc 1
"$T/live/dropbear" 600 & LIVE=$!; echo "$LIVE" > "$SSH_PIDFILE"
echo 1 > "$SSH_PIDFILE"   # pid 1 is not dropbear
run remove-startssh
check "a stale pid file is not a live dropbear" rc 1
echo "$LIVE" > "$SSH_PIDFILE"

echo "remove (uninstall)"
fresh; run add
rm -f "$T/calls"; run remove
check "succeeds" rc 0
check "takes the launcher line out" test "$(count "$LAUNCHER")" = 0
check "keeps the other lines, including startssh" test "$(cat "$CRON_DIR/root")" = "$ORIGINAL"
check "one writable window" test "$(calls)" = "mntroot rw mntroot ro "
rm -f "$T/calls"; run remove
check "removing again is a no-op" rc 0
check "and does not open the root filesystem" test ! -e "$T/calls"
fresh; run add; chmod a-w "$CRON_DIR/root"; rm -f "$T/calls"; run remove
check "a table that cannot be written is left whole" test "$(count "$ENTRY")" = 1
check "and reports failure" rc 1
chmod u+w "$CRON_DIR/root"

echo "remove-startssh alone"
fresh; run remove-startssh
check "succeeds" rc 0
check "removes only the startssh line" test "$(cat "$CRON_DIR/root")" = "$(printf '%s\n' "$ORIGINAL" | grep -v startssh.sh)"
check "does not add the launcher" test "$(count "$LAUNCHER")" = 0

echo "a table with no newline at the end"
printf '%s' "$ORIGINAL" > "$CRON_DIR/root"; rm -f "$T/calls"; run add
check "gets the entry on a line of its own" test "$(tail -n 1 "$CRON_DIR/root")" = "$ENTRY"
check "and the last old line is intact" test "$(count '* * * * * /usr/bin/startssh.sh > /dev/null 2>&1')" = 1

echo "mntroot rw fails"
fresh; export MNTROOT_FAIL=rw; run add
check "fails" rc 1
check "says so" grep -q 'nothing was changed' "$T/out"
check "leaves the table alone" test "$(cat "$CRON_DIR/root")" = "$ORIGINAL"
unset MNTROOT_FAIL

echo "the table cannot be written"
fresh; chmod a-w "$CRON_DIR/root"; run add
check "fails" rc 1
check "puts back / leaves the original" test "$(cat "$CRON_DIR/root")" = "$ORIGINAL"
check "still returns the filesystem to read-only" test "$(calls)" = "mntroot rw mntroot ro "
chmod u+w "$CRON_DIR/root"

echo "mntroot ro fails afterwards"
fresh; export MNTROOT_FAIL=ro; run add
check "the edit still succeeded" test "$(count "$ENTRY")" = 1
check "but it warns, loudly" grep -q 'WARNING' "$T/out"
unset MNTROOT_FAIL

echo "no crontab"
rm -f "$CRON_DIR/root"; run add
check "fails cleanly" rc 1
check "names the file" grep -q "$CRON_DIR/root" "$T/out"

echo "finding crond's directory from the process list"
mkdir -p "$T/other"; printf '%s\n' "$ORIGINAL" > "$T/other/root"
cat > "$T/bin/ps" <<EOF
#!/bin/sh
echo "root       376     1  0 03:16 ?        00:00:00 crond -f -c $T/other"
EOF
chmod +x "$T/bin/ps"
out="$(env -u CRON_DIR PATH="$T/bin:$PATH" sh "$SCRIPT" status 2>&1)"
check "uses the directory crond was started with" sh -c "echo '$out' | grep -q 'table: $T/other/root'"
rm -f "$T/bin/ps"

echo "an unknown command"
fresh; run frobnicate
check "is refused with usage" sh -c "grep -q usage $T/out && [ \"\$(cat $T/rc)\" = 1 ]"

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
