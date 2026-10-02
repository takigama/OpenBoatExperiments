#!/usr/bin/env bash
# Exercises install.sh (and the on-device uninstaller) against a stand-in
# repository served over file://, a stand-in FBInk and stand-in launcher
# scripts: device detection and matching, the "no profile" stop, --check, the
# checksum refusals, atomic install, idempotent re-runs, uninstall, and the
# PC (--host) mode. Nothing here needs a Kindle.
#
#   bash scripts/test_install.sh
set -u
cd "$(dirname "$0")/.."
ROOT="$PWD"
INSTALL="$ROOT/install.sh"

T="$(mktemp -d)"
trap 'chmod -R u+w "$T" 2>/dev/null; rm -rf "$T"' EXIT
REPO="$T/repo"                      # what GitHub would serve: .../SignalKPaperDisplay/
mkdir -p "$REPO/platforms/kindle-basic/install" "$REPO/platforms/kindle-pw3/install" \
         "$REPO/tools/fbink/prebuilt" "$REPO/update" "$REPO/releases" "$T/bin"

# The real descriptions of the platforms and the real uninstaller; stand-ins
# for the things that would touch a Kindle (the launcher and the cron editor).
cp "$ROOT/platforms/devices.txt" "$REPO/platforms/"
cp "$ROOT/platforms/kindle-basic/profile.json" "$REPO/platforms/kindle-basic/"
cp "$ROOT/platforms/kindle-pw3/profile.json" "$REPO/platforms/kindle-pw3/"
cp "$ROOT/platforms/kindle-pw3/install/device-uninstall.sh" "$REPO/platforms/kindle-pw3/install/"
printf 'SIGNALK_HOST=10.0.0.1:1\nSTOP_JOBS="shared"\n' > "$REPO/platforms/kindle-pw3/install/launcher.conf.example"
printf 'SIGNALK_HOST=10.0.0.1:1\nSTOP_JOBS="basic own"\n' > "$REPO/platforms/kindle-basic/install/launcher.conf.example"
cat > "$REPO/platforms/kindle-pw3/install/startpaper.sh" <<'EOF'
#!/bin/sh
echo "startpaper ran (disable=$([ -f "$DIR/disable" ] && echo yes || echo no))" >> "$DIR/calls"
EOF
cat > "$REPO/platforms/kindle-pw3/install/install-cron.sh" <<'EOF'
#!/bin/sh
echo "install-cron $*" >> "$DIR/calls"
case "$1" in status) echo "startssh.sh cron entry: present" ;; esac
[ -z "${CRON_FAIL:-}" ]
EOF

# A fake app and a fake FBInk, with the checksums the manifest and .sha256 publish.
printf '#!/bin/sh\necho app\n' > "$REPO/releases/paperdisplay-kindle-basic"
printf '#!/bin/sh\necho app pw3\n' > "$REPO/releases/paperdisplay-kindle-pw3"
printf '#!/bin/sh\necho fbink\n' > "$REPO/tools/fbink/prebuilt/fbink-kindlepw2"
sum() { sha256sum "$1" | cut -d' ' -f1; }
write_manifest() { # basic-sum pw3-sum
  cat > "$REPO/update/manifest.json" <<EOF
{
  "platforms": {
    "kindle-basic": { "version": 7, "url": "file://$REPO/releases/paperdisplay-kindle-basic", "sha256": "$1" },
    "kindle-pw3": { "version": 7, "url": "file://$REPO/releases/paperdisplay-kindle-pw3", "sha256": "$2" }
  }
}
EOF
}
good_manifest() { write_manifest "$(sum "$REPO/releases/paperdisplay-kindle-basic")" "$(sum "$REPO/releases/paperdisplay-kindle-pw3")"; }
good_fbink_sum() { echo "$(sum "$REPO/tools/fbink/prebuilt/fbink-kindlepw2")  fbink-kindlepw2" > "$REPO/tools/fbink/prebuilt/fbink-kindlepw2.sha256"; }
good_manifest; good_fbink_sum

# A fake `fbink -e`: the codename and size come from the environment.
cat > "$T/bin/fbink" <<'EOF'
#!/bin/sh
[ "$1" = -e ] || exit 0
echo "[FBInk] Detected a fake" >&2
echo "FBINK_VERSION='v1.25.0';viewWidth=${FAKE_W:-600};viewHeight=${FAKE_H:-800};DPI=167;deviceName='${FAKE_NAME:-Basic 2}';deviceCodename='${FAKE_CODE:-Eanab}';devicePlatform='X';"
EOF
chmod +x "$T/bin/fbink"
printf 'G000XXXXXXXXXXXX' > "$T/usid"

export INSTALL_BASE="file://$REPO" FBINK_BIN="$T/bin/fbink" INSTALL_USID="$T/usid"
export INSTALL_START_WAIT=0 UNINSTALL_WAIT=0 TMPDIR="$T"
DIR="$T/app"; export KINDLE_DIR="$DIR"

pass=0; fail=0
check() { local d="$1"; shift; if "$@"; then pass=$((pass+1)); echo "  ok   $d"; else fail=$((fail+1)); echo "  FAIL $d"; fi; }
# setsid: no controlling terminal, as over a plain ssh command, so it never stops to prompt.
nt() { setsid -w "$@"; }
run() { nt sh "$INSTALL" "$@" > "$T/out" 2>&1 < /dev/null; echo $? > "$T/rc"; }
rc() { [ "$(cat "$T/rc")" = "$1" ]; }
out() { grep -q -- "$1" "$T/out"; }
fresh() { rm -rf "$DIR"; unset CRON_FAIL; }

echo "help and bad options"
run --help
check "--help prints the usage" out "usage:"
check "and succeeds" rc 0
run --frobnicate
check "an unknown option is refused" rc 2
run --signalk 'a;b'
check "a SignalK value with odd characters is refused" rc 1
run --dir '/tmp/a b' --check
check "a directory with a space is refused" rc 1
run --keep-ssh
check "--keep-ssh alone is refused" rc 1
run --uninstall --keep-ssh --purge
check "--keep-ssh with --purge is refused" rc 1

echo "recognising the Kindle"
fresh; run --check
check "the Eanab codename finds kindle-basic" out "found: kindle-basic"
check "reports what the Kindle is" out "codename Eanab"
check "reports the build it would install" out "kindle-basic build 7"
check "says nothing was changed" out "nothing was changed"
check "--check succeeds" rc 0
check "--check creates nothing" test ! -e "$DIR"
check "reports only four characters of the serial" sh -c "grep -q 'G000 (first four' $T/out && ! grep -q 'XXXX' $T/out"

FAKE_CODE=Other FAKE_NAME=Other run --check
check "an unknown Kindle gets no profile" out "none: there is no profile"
check "and exits with 3" rc 3
check "and is told how to get one made" out "kindle-probe.sh"
check "and nothing is changed" test ! -e "$DIR"

printf 'G090ZZZZ' > "$T/usid"
FAKE_CODE=Other FAKE_W=1072 FAKE_H=1448 run --check
check "serial G090 at 1072x1448 finds kindle-pw3" out "found: kindle-pw3"
FAKE_CODE=Other FAKE_W=600 FAKE_H=800 run --check
check "serial G090 at another size does not match" rc 3
printf 'G000XXXXXXXXXXXX' > "$T/usid"

FAKE_CODE=Other run --check --platform kindle-basic
check "--platform names the profile for a Kindle with none" out "using kindle-basic, as you asked"
check "and that succeeds" rc 0
run --check --platform kindle-pw3
check "--platform differing from the detected one warns" out "looks like kindle-basic, but you asked for kindle-pw3"
run --check --platform nosuch
check "a profile that does not exist is an error" rc 1
check "and says so" out "no profile named nosuch"

echo "installing"
fresh; run --signalk 192.168.1.20:3000
check "succeeds" rc 0
for f in paperdisplay fbink profile.json startpaper.sh install-cron.sh uninstall.sh launcher.conf.example launcher.conf; do
  check "installs $f" test -f "$DIR/$f"
done
check "the app is executable" test -x "$DIR/paperdisplay"
check "the app is the published one" cmp -s "$DIR/paperdisplay" "$REPO/releases/paperdisplay-kindle-basic"
check "FBInk is the published one" cmp -s "$DIR/fbink" "$REPO/tools/fbink/prebuilt/fbink-kindlepw2"
check "profile.json is kindle-basic's" grep -q '"name": "kindle-basic"' "$DIR/profile.json"
check "uninstall.sh is the device uninstaller" grep -q 'giving the stock UI back' "$DIR/uninstall.sh"
check "launcher.conf has the SignalK server" grep -q '^SIGNALK_HOST=192.168.1.20:3000$' "$DIR/launcher.conf"
check "and the platform's own example, not the shared one" grep -q 'basic own' "$DIR/launcher.conf"
check "adds the launcher to cron" grep -q 'install-cron add' "$DIR/calls"
check "starts the launcher" grep -q 'startpaper ran' "$DIR/calls"
check "leaves no scratch directory behind" sh -c "! ls -A $DIR | grep -q '^\.install'"
check "leaves no temporary files behind" sh -c "! ls $T | grep -q kindle-install"
check "mentions the startssh job still in cron" out "still has a startssh.sh job"

echo "running it again"
echo 'EXTRA_ARGS="-x"' >> "$DIR/launcher.conf"
run
check "succeeds" rc 0
check "says it is an update" sh -c "run() { :; }; sh '$INSTALL' --check < /dev/null 2>&1 | grep -q 'already installed there'"
check "leaves launcher.conf alone" grep -q 'EXTRA_ARGS="-x"' "$DIR/launcher.conf"
check "says so" out "already exists: left alone"
run --signalk other.host:99
check "a new --signalk changes only that line" sh -c "grep -q '^SIGNALK_HOST=other.host:99$' $DIR/launcher.conf && grep -q 'EXTRA_ARGS=\"-x\"' $DIR/launcher.conf"
run --remove-startssh --no-start
check "--remove-startssh asks the cron editor" grep -q 'install-cron remove-startssh' "$DIR/calls"

echo "options that skip steps"
fresh; run --signalk h:1 --no-cron --no-start
check "--no-cron does not call the cron editor" sh -c "! grep -q install-cron $DIR/calls 2>/dev/null"
check "--no-start does not start it" sh -c "! grep -q startpaper $DIR/calls 2>/dev/null"
check "but the files are there" test -f "$DIR/paperdisplay"
fresh; run
check "with no SignalK server and no launcher.conf, it installs the files" test -f "$DIR/paperdisplay"
check "does not write a launcher.conf" test ! -e "$DIR/launcher.conf"
check "does not start the app" sh -c "! grep -q startpaper $DIR/calls 2>/dev/null"
check "says how to give it the server" out "--signalk HOST:PORT"
fresh; CRON_FAIL=1 run --signalk h:1
check "a failing cron editor does not stop the install" rc 0
check "but it is reported" out "the crontab was not changed"

echo "refusing bad downloads"
fresh; write_manifest "$(printf 0%.0s $(seq 64))" "$(sum "$REPO/releases/paperdisplay-kindle-pw3")"; run --signalk h:1
check "an app that does not match its published checksum is refused" out "does not match its published sha256"
check "with a failure status" rc 1
check "and nothing is installed" sh -c "[ ! -e $DIR/paperdisplay ] && [ ! -e $DIR/fbink ] && [ ! -e $DIR/launcher.conf ]"
good_manifest
fresh; echo "$(printf 0%.0s $(seq 64))  fbink-kindlepw2" > "$REPO/tools/fbink/prebuilt/fbink-kindlepw2.sha256"; run --signalk h:1
check "FBInk that does not match is refused" out "FBInk does not match"
check "and nothing is installed" sh -c "[ ! -e $DIR/paperdisplay ] && [ ! -e $DIR/fbink ]"
good_fbink_sum
fresh; mv "$REPO/platforms/kindle-pw3/install/startpaper.sh" "$T/hold"; run --signalk h:1
check "a file that cannot be downloaded stops the install" rc 1
check "and says which" out "could not download startpaper.sh"
check "and nothing is installed" sh -c "[ ! -e $DIR/paperdisplay ] && [ ! -e $DIR/fbink ]"
mv "$T/hold" "$REPO/platforms/kindle-pw3/install/startpaper.sh"
sed -i 's|https://github.com|file://x|; s|"url": "file://|"url": "http://|' "$REPO/update/manifest.json"
fresh; run --signalk h:1
check "a download address that is not GitHub is refused" out "unexpected download address"
good_manifest
fresh; sed -i '/kindle-basic/d' "$REPO/update/manifest.json"; run --check
check "a platform with no release is reported" out "no released build of kindle-basic"
good_manifest

echo "an update over a running app"
fresh; run --signalk h:1
printf '#!/bin/sh\necho app v2\n' > "$REPO/releases/paperdisplay-kindle-basic"; good_manifest
# Hold the old app open for writing as a running program would.
( exec 3< "$DIR/paperdisplay"; sleep 2 ) &
run
check "replacing the app succeeds" rc 0
check "and it is the new one" cmp -s "$DIR/paperdisplay" "$REPO/releases/paperdisplay-kindle-basic"
wait

echo "the Kindle's own uninstaller"
fresh; run --signalk h:1
run --uninstall
check "succeeds" rc 0
check "switches the launcher off first" grep -q 'startpaper ran (disable=yes)' "$DIR/calls"
check "removes the launcher from cron" grep -q 'install-cron remove' "$DIR/calls"
check "removes the disable file again" test ! -e "$DIR/disable"
check "keeps the files and the settings" sh -c "[ -f $DIR/paperdisplay ] && [ -f $DIR/launcher.conf ]"
check "says the Kindle is back on its own software" out "running its own software again"
fresh; run --signalk h:1
run --uninstall --keep-ssh
check "--keep-ssh succeeds" rc 0
check "--keep-ssh leaves the cron entry" sh -c "! grep -q 'install-cron remove' $DIR/calls"
check "--keep-ssh leaves the app switched off" test -f "$DIR/disable"
fresh; run --signalk h:1
run --uninstall --purge
check "--purge succeeds" rc 0
check "--purge deletes the directory" test ! -e "$DIR"
run --uninstall
check "with nothing installed it says so" out "Nothing to remove"
check "and succeeds" rc 0
fresh; run --signalk h:1; CRON_FAIL=1 run --uninstall
check "a cron editor that fails makes the uninstall fail" rc 1
check "and the app stays switched off" test -f "$DIR/disable"
fresh; run --signalk h:1
cp "$DIR/uninstall.sh" "$T/u.sh"; mv "$REPO/platforms/kindle-pw3/install/device-uninstall.sh" "$T/hold"
run --uninstall
check "if it cannot be downloaded, the installed copy is used" rc 0
mv "$T/hold" "$REPO/platforms/kindle-pw3/install/device-uninstall.sh"

echo "run from a pipe, as curl would"
fresh
nt sh -s -- --check < "$INSTALL" > "$T/out" 2>&1; echo $? > "$T/rc"
check "works with the script on stdin" out "found: kindle-basic"
check "and exits cleanly" rc 0
fresh
nt sh -s -- --signalk h:1 --no-start < "$INSTALL" > "$T/out" 2>&1; echo $? > "$T/rc"
check "an install from stdin finishes, the later commands still running" out "Done. To change settings"
fresh; head -c 3000 "$INSTALL" | nt sh -s -- --signalk h:1 > "$T/out" 2>&1
check "a download cut short installs nothing" test ! -e "$DIR"

echo "from a PC (--host)"
cat > "$T/bin/ssh" <<'EOF'
#!/bin/sh
echo "$*" > "$SSH_LOG"
cat > "$SSH_STDIN"
EOF
chmod +x "$T/bin/ssh"
export SSH_LOG="$T/ssh.args" SSH_STDIN="$T/ssh.stdin"
PATH="$T/bin:$PATH" sh "$INSTALL" --host 10.1.2.3 --port 22 --signalk h:1 --check > "$T/out" 2>&1
check "ssh goes to the host and port given" grep -q -- '-p 22 .*root@10.1.2.3' "$T/ssh.args"
check "runs the installer with the options, but not the ssh ones" grep -q "sh -s -- --signalk h:1 --check\$" "$T/ssh.args"
check "sends this very script over ssh" cmp -s "$T/ssh.stdin" "$INSTALL"
check "passes the test base on" grep -q "INSTALL_BASE='file://" "$T/ssh.args"
PATH="$T/bin:$PATH" sh -s -- --host 10.1.2.3 --user admin --ref v9 < "$INSTALL" > "$T/out" 2>&1
check "from a pipe, the Kindle fetches the script itself" grep -q "curl -fsSL 'https://raw.githubusercontent.com/takigama/OpenBoatExperiments/v9/SignalKPaperDisplay/install.sh'" "$T/ssh.args"
check "as the user given" grep -q 'admin@10.1.2.3' "$T/ssh.args"
PATH="$T/bin:$PATH" sh "$INSTALL" --host 'a;b' > "$T/out" 2>&1; echo $? > "$T/rc"
check "a host with odd characters is refused" rc 1

echo "the PC uninstall wrapper"
cat > "$T/bin/ssh" <<'EOF'
#!/bin/sh
echo "$*" > "$SSH_LOG"
cat > "$SSH_STDIN"
EOF
PATH="$T/bin:$PATH" bash "$ROOT/platforms/kindle-pw3/install/uninstall.sh" 10.9.9.9 2223 --purge > "$T/out" 2>&1
check "sends the device uninstaller over ssh" cmp -s "$T/ssh.stdin" "$ROOT/platforms/kindle-pw3/install/device-uninstall.sh"
check "with its option and the install directory" grep -q "DIR='$DIR' sh -s -- --purge" "$T/ssh.args"
PATH="$T/bin:$PATH" bash "$ROOT/platforms/kindle-basic/install/uninstall.sh" > "$T/out" 2>&1; echo $? > "$T/rc"
check "the 8th-gen wrapper still demands a host" rc 2

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
