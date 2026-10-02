#!/usr/bin/env bash
# Copies the app to the Kindle over SSH. Run it yourself: ssh asks for the
# password once, and nothing here ever stores or sends it.
#
#   bash platforms/kindle-pw3/install/deploy.sh [host] [port]
#
# Defaults: 10.0.0.164, port 2223 (KOReader's Dropbear). Override with
# arguments or KINDLE_HOST / KINDLE_PORT / KINDLE_USER / KINDLE_DIR.
#
# Everything goes in ONE ssh connection by streaming a tar archive, so there
# is a single password prompt and no need for scp/sftp on the device (they're
# usually missing from Dropbear setups). Only files this project owns are
# written, under KINDLE_DIR - FBInk and your other files are left alone.

set -eu

HOST="${1:-${KINDLE_HOST:-10.0.0.164}}"
PORT="${2:-${KINDLE_PORT:-2223}}"
USER="${KINDLE_USER:-root}"
DIR="${KINDLE_DIR:-/mnt/us/signalk}"

HERE="$(cd "$(dirname "$0")" && pwd)"
PLATFORM="$(dirname "$HERE")"
ROOT="$(cd "$PLATFORM/../.." && pwd)"
BIN="$ROOT/dist/kindle-pw3/paperdisplay"

if [ ! -f "$BIN" ]; then
  echo "Missing $BIN - build it first:  make kindle-pw3" >&2
  exit 1
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp "$BIN" "$STAGE/paperdisplay"
cp "$PLATFORM/profile.json" "$STAGE/profile.json"
# A full FBInk (with image support) if it's been built: bash tools/fbink/build.sh
# KOReader's bundled fbink can't draw images, so we ship our own beside the app.
FBINK="$ROOT/dist/fbink/fbink-kindlepw2"
[ -f "$FBINK" ] && cp "$FBINK" "$STAGE/fbink"
# Launcher/helper scripts that live next to this one (other than deploy.sh).
for f in "$HERE"/*.sh; do
  [ "$(basename "$f")" = "deploy.sh" ] && continue
  [ -f "$f" ] && cp "$f" "$STAGE/"
done
# Example config (launcher.conf.example) - never launcher.conf itself, so a
# deploy can't overwrite the device's real settings.
for f in "$HERE"/*.example; do
  [ -f "$f" ] && cp "$f" "$STAGE/"
done
chmod +x "$STAGE/paperdisplay" "$STAGE"/*.sh 2>/dev/null || true
[ -f "$STAGE/fbink" ] && chmod +x "$STAGE/fbink"
# What we checksum on both ends: only files this run actually shipped.
SUMMED="paperdisplay profile.json"
[ -f "$STAGE/fbink" ] && SUMMED="$SUMMED fbink"

echo "Deploying to $USER@$HOST:$PORT  ->  $DIR"
(cd "$STAGE" && ls -l)
echo

# One connection: unpack, then print checksums of what landed so we can
# confirm the copy is byte-identical.
REMOTE="mkdir -p '$DIR' && tar -xf - -C '$DIR' && cd '$DIR' && chmod +x $SUMMED *.sh 2>/dev/null; md5sum $SUMMED *.sh 2>/dev/null"

# "|| true": a missing optional file makes md5sum exit non-zero, which must
# not abort the script before the comparison below decides pass/fail.
REMOTE_SUMS="$(tar -cf - -C "$STAGE" . | ssh -p "$PORT" \
  -o StrictHostKeyChecking=accept-new \
  "$USER@$HOST" "$REMOTE" || true)"

LOCAL_SUMS="$(cd "$STAGE" && md5sum $SUMMED *.sh 2>/dev/null || true)"

if [ "$(echo "$REMOTE_SUMS" | sort)" = "$(echo "$LOCAL_SUMS" | sort)" ]; then
  echo "OK - checksums match:"
  echo "$REMOTE_SUMS"
else
  echo "MISMATCH between local and device checksums!" >&2
  echo "--- local"  >&2; echo "$LOCAL_SUMS"  >&2
  echo "--- device" >&2; echo "$REMOTE_SUMS" >&2
  exit 1
fi
