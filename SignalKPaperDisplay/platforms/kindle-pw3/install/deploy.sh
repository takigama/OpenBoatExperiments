#!/usr/bin/env bash
# Installs the app on a Kindle over SSH, start to finish. Run it yourself: ssh
# asks for the password once (or uses your key), and nothing here ever stores
# or sends it.
#
#   bash platforms/kindle-pw3/install/deploy.sh [host] [port] [options]
#   bash platforms/kindle-basic/install/deploy.sh <host> [port] [options]
#
# It: builds what is missing (the app, and FBInk), copies the files and checks
# their checksums, writes launcher.conf (asking for your SignalK server), adds
# the launcher to the Kindle's crontab, starts it, and shows you the result.
# Every step is safe to repeat, so run it again to update the files.
#
# Options:
#   --signalk HOST:PORT   the SignalK server; written to launcher.conf (and
#                         changed there if it already exists). Asked for if
#                         there is no launcher.conf yet and you are at a terminal.
#   --remove-startssh     also remove an old hand-made startssh.sh cron job,
#                         once the launcher has been seen to keep ssh alive
#   --no-cron             do not touch the Kindle's crontab
#   --no-start            do not start the launcher at the end
#   --files-only          only copy the files: no config, no cron, no start
#   -h, --help            this text
#
# Defaults: 10.0.0.164, port 2223 (KOReader's Dropbear). Override with
# arguments or KINDLE_HOST / KINDLE_PORT / KINDLE_USER / KINDLE_DIR.
#
# Everything goes through ONE ssh connection (it is shared between the steps),
# so there is a single password prompt, and no need for scp/sftp on the device.
# Only files this project owns are written, under KINDLE_DIR - FBInk and your
# other files are left alone. To undo it all: uninstall.sh, beside this one.

set -eu

usage() { sed -n '2,/^# Everything goes/p' "$0" | sed 's/^# \{0,1\}//' | sed '$d'; }

HOST_ARG=""; PORT_ARG=""; SIGNALK=""
DO_CRON=1; DO_START=1; FILES_ONLY=0; RM_STARTSSH=0
while [ $# -gt 0 ]; do
  case "$1" in
    --signalk)         SIGNALK="${2:?--signalk wants HOST:PORT}"; shift 2 ;;
    --signalk=*)       SIGNALK="${1#*=}"; shift ;;
    --remove-startssh) RM_STARTSSH=1; shift ;;
    --no-cron)         DO_CRON=0; shift ;;
    --no-start)        DO_START=0; shift ;;
    --files-only)      FILES_ONLY=1; shift ;;
    -h|--help)         usage; exit 0 ;;
    -*)                echo "unknown option: $1" >&2; echo >&2; usage >&2; exit 2 ;;
    *)
      if   [ -z "$HOST_ARG" ]; then HOST_ARG="$1"
      elif [ -z "$PORT_ARG" ]; then PORT_ARG="$1"
      else echo "unexpected argument: $1" >&2; exit 2; fi
      shift ;;
  esac
done

# A platform's own deploy.sh can demand an address (so it never quietly falls
# back to the Paperwhite 3's).
if [ -n "${DEPLOY_REQUIRE_HOST:-}" ] && [ -z "$HOST_ARG" ] && [ -z "${KINDLE_HOST:-}" ]; then
  echo "usage: $0 <host> [port] [options]   (--help for the options)" >&2
  exit 2
fi

HOST="${HOST_ARG:-${KINDLE_HOST:-10.0.0.164}}"
PORT="${PORT_ARG:-${KINDLE_PORT:-2223}}"
USER="${KINDLE_USER:-root}"
DIR="${KINDLE_DIR:-/mnt/us/signalk}"

if [ -n "$SIGNALK" ] && ! printf '%s' "$SIGNALK" | grep -Eq '^[A-Za-z0-9._-]+(:[0-9]{1,5})?$'; then
  echo "--signalk wants HOST or HOST:PORT (letters, digits, dots and dashes), got: $SIGNALK" >&2
  exit 2
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
# Another Kindle's own deploy.sh can reuse this one: it sets DEPLOY_PLATFORM_DIR
# to its directory (for the profile and the binary's name) and keeps the shared
# launcher scripts here. On its own this deploys the Paperwhite 3.
PLATFORM="${DEPLOY_PLATFORM_DIR:-$(dirname "$HERE")}"
ROOT="$(cd "$PLATFORM/../.." && pwd)"
NAME="$(basename "$PLATFORM")"
BIN="$ROOT/dist/$NAME/paperdisplay"
FBINK="$ROOT/dist/fbink/fbink-kindlepw2"

# --- build what is missing ----------------------------------------------------
if [ ! -f "$BIN" ]; then
  echo "== building $NAME (this needs docker) =="
  (cd "$ROOT" && make "$NAME") || { echo "the build failed: fix it, or run 'make $NAME' yourself" >&2; exit 1; }
fi
if [ ! -f "$FBINK" ]; then
  # A full FBInk (with image support): KOReader's bundled one can't draw images.
  echo "== building FBInk (a few minutes, once) =="
  (cd "$ROOT" && bash tools/fbink/build.sh) || { echo "the FBInk build failed" >&2; exit 1; }
fi

# --- one ssh connection, shared by every step ---------------------------------
CTL="/tmp/kdeploy-$$"
SSH_OPTS=(-p "$PORT" -o StrictHostKeyChecking=accept-new
          -o ControlMaster=auto -o "ControlPath=$CTL" -o ControlPersist=300)
remote() { ssh "${SSH_OPTS[@]}" "$USER@$HOST" "$@"; }

STAGE="$(mktemp -d)"
cleanup() { ssh "${SSH_OPTS[@]}" -O exit "$USER@$HOST" >/dev/null 2>&1 || true; rm -rf "$STAGE"; }
trap cleanup EXIT

# --- 1. copy the files --------------------------------------------------------
cp "$BIN" "$STAGE/paperdisplay"
cp "$PLATFORM/profile.json" "$STAGE/profile.json"
cp "$FBINK" "$STAGE/fbink"
# Launcher/helper scripts that live next to this one (other than deploy.sh and
# uninstall.sh, which run on your PC).
for f in "$HERE"/*.sh; do
  case "$(basename "$f")" in deploy.sh|uninstall.sh) continue ;; esac
  [ -f "$f" ] && cp "$f" "$STAGE/"
done
# Example config (launcher.conf.example) - never launcher.conf itself, so a
# deploy can't overwrite the device's real settings.
for f in "$HERE"/*.example; do
  [ -f "$f" ] && cp "$f" "$STAGE/"
done
# A platform's own example config, if it has one, replaces the shared one: the
# stock UI jobs to stop differ between Kindles.
if [ "$PLATFORM/install" != "$HERE" ]; then
  for f in "$PLATFORM/install"/*.example; do
    [ -f "$f" ] && cp "$f" "$STAGE/"
  done
fi
chmod +x "$STAGE/paperdisplay" "$STAGE/fbink" "$STAGE"/*.sh 2>/dev/null || true
# What we checksum on both ends: only files this run actually shipped.
SUMMED="paperdisplay profile.json fbink"

echo "== 1. copying to $USER@$HOST:$PORT  ->  $DIR =="
(cd "$STAGE" && ls -l)
echo

# Unpack, then print checksums of what landed so we can confirm the copy is
# byte-identical. Unpacked beside the real files and then moved into place, one rename each:
# writing over a program that is running (the app, or the fbink it calls every
# second) fails with "text file busy" or catches it half-written, and a rename
# never does.
REMOTE="mkdir -p '$DIR' && S='$DIR/.deploy.$$' && rm -rf \"\$S\" && mkdir \"\$S\" && tar -xf - -C \"\$S\" && chmod +x \"\$S\"/* 2>/dev/null; for f in \"\$S\"/*; do mv -f \"\$f\" '$DIR'/; done; rmdir \"\$S\"; cd '$DIR' && md5sum $SUMMED *.sh 2>/dev/null"

# "|| true": a missing optional file makes md5sum exit non-zero, which must
# not abort the script before the comparison below decides pass/fail.
REMOTE_SUMS="$(tar -cf - -C "$STAGE" . | ssh "${SSH_OPTS[@]}" "$USER@$HOST" "$REMOTE" || true)"
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

if [ "$FILES_ONLY" = 1 ]; then
  echo
  echo "Files copied. Run again without --files-only to configure and start it."
  exit 0
fi

# --- 2. launcher.conf ---------------------------------------------------------
echo
echo "== 2. configuration =="
HAVE_CONF=0
remote "[ -f '$DIR/launcher.conf' ]" && HAVE_CONF=1

if [ -z "$SIGNALK" ] && [ "$HAVE_CONF" = 0 ] && [ -t 0 ]; then
  read -r -p "SignalK server (host or host:port, e.g. 192.168.1.20:3000): " SIGNALK
  if [ -n "$SIGNALK" ] && ! printf '%s' "$SIGNALK" | grep -Eq '^[A-Za-z0-9._-]+(:[0-9]{1,5})?$'; then
    echo "that is not a host or host:port: $SIGNALK" >&2; exit 2
  fi
fi

if [ "$HAVE_CONF" = 1 ]; then
  if [ -n "$SIGNALK" ]; then
    remote "cd '$DIR' && sed -i 's|^SIGNALK_HOST=.*|SIGNALK_HOST=$SIGNALK|' launcher.conf"
    echo "launcher.conf: SignalK server set to $SIGNALK (the rest is as you had it)"
  else
    echo "launcher.conf already exists: left alone"
  fi
elif [ -n "$SIGNALK" ]; then
  remote "cd '$DIR' && sed 's|^SIGNALK_HOST=.*|SIGNALK_HOST=$SIGNALK|' launcher.conf.example > launcher.conf"
  echo "launcher.conf written from the example, with SignalK at $SIGNALK"
  HAVE_CONF=1
else
  echo "No SignalK server given, so launcher.conf was not written."
  echo "  Run again with  --signalk HOST:PORT  or copy launcher.conf.example yourself."
fi

# --- 3. cron ------------------------------------------------------------------
echo
echo "== 3. start at boot (crontab) =="
if [ "$DO_CRON" = 1 ]; then
  remote "DIR='$DIR' sh '$DIR/install-cron.sh' add"
else
  echo "skipped (--no-cron): the launcher will not run by itself"
fi

# --- 4. start -----------------------------------------------------------------
echo
echo "== 4. starting =="
if [ "$DO_START" = 1 ] && [ "$HAVE_CONF" = 1 ]; then
  remote "cd '$DIR' && sh '$DIR/startpaper.sh'; sleep 10; echo '--- log'; tail -12 '$DIR/paperdisplay.log' 2>/dev/null; echo '---'; if pidof paperdisplay >/dev/null; then echo 'RUNNING: paperdisplay is up'; else echo 'NOT RUNNING: see the log above'; fi"
elif [ "$HAVE_CONF" = 0 ]; then
  echo "not started: there is no launcher.conf yet"
else
  echo "not started (--no-start)"
fi

# --- 5. the old ssh job -------------------------------------------------------
if [ "$RM_STARTSSH" = 1 ] && [ "$DO_CRON" = 1 ]; then
  echo
  echo "== 5. the old startssh.sh job =="
  remote "DIR='$DIR' sh '$DIR/install-cron.sh' remove-startssh"
elif [ "$DO_CRON" = 1 ]; then
  if remote "DIR='$DIR' sh '$DIR/install-cron.sh' status" 2>/dev/null | grep -q 'startssh.sh cron entry: present'; then
    echo
    echo "Note: your crontab still has a startssh.sh job. The launcher keeps ssh alive"
    echo "now, so it is no longer needed: run this again with --remove-startssh to take it out."
  fi
fi

echo
echo "Done. To change settings later, tap the cog on the screen. To take it all off the"
echo "Kindle again: bash $(dirname "$0")/uninstall.sh $HOST $PORT"
