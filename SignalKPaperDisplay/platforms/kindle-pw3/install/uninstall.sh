#!/usr/bin/env bash
# Takes the app off a Kindle again, over SSH, and gives it back to the stock
# software. Run it yourself: ssh asks for the password once (or uses your key).
#
#   bash platforms/kindle-pw3/install/uninstall.sh [host] [port] [options]
#   bash platforms/kindle-basic/install/uninstall.sh <host> [port] [options]
#
# It: gives the stock UI back (stops the app and starts the jobs the launcher
# had stopped), then removes the launcher from the Kindle's crontab. The files
# stay in place, so putting it back later (deploy.sh) keeps your settings.
#
# Options:
#   --keep-ssh   leave the launcher in the crontab, switched off with a `disable`
#                file: the stock UI is back, and the launcher goes on doing the
#                one thing the stock software won't, keeping ssh running
#   --purge      also delete everything under /mnt/us/signalk, settings and logs
#                included (not allowed with --keep-ssh, which needs the launcher)
#   -h, --help   this text
#
# Defaults: 10.0.0.164, port 2223. Override with arguments or KINDLE_HOST /
# KINDLE_PORT / KINDLE_USER / KINDLE_DIR.
#
# The launcher is what keeps ssh running, so without --keep-ssh it stays up
# only until the Kindle next reboots; start it again from KOReader's menu
# (Network, SSH server) when you need it.

set -eu

usage() { sed -n '2,/^# The launcher is what/p' "$0" | sed 's/^# \{0,1\}//' | sed '$d'; }

HOST_ARG=""; PORT_ARG=""; KEEP_SSH=0; PURGE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --keep-ssh) KEEP_SSH=1; shift ;;
    --purge)    PURGE=1; shift ;;
    -h|--help)  usage; exit 0 ;;
    -*)         echo "unknown option: $1" >&2; echo >&2; usage >&2; exit 2 ;;
    *)
      if   [ -z "$HOST_ARG" ]; then HOST_ARG="$1"
      elif [ -z "$PORT_ARG" ]; then PORT_ARG="$1"
      else echo "unexpected argument: $1" >&2; exit 2; fi
      shift ;;
  esac
done

if [ -n "${DEPLOY_REQUIRE_HOST:-}" ] && [ -z "$HOST_ARG" ] && [ -z "${KINDLE_HOST:-}" ]; then
  echo "usage: $0 <host> [port] [options]   (--help for the options)" >&2
  exit 2
fi
if [ "$KEEP_SSH" = 1 ] && [ "$PURGE" = 1 ]; then
  echo "--purge deletes the launcher, so it cannot be combined with --keep-ssh" >&2
  exit 2
fi

HOST="${HOST_ARG:-${KINDLE_HOST:-10.0.0.164}}"
PORT="${PORT_ARG:-${KINDLE_PORT:-2223}}"
USER="${KINDLE_USER:-root}"
DIR="${KINDLE_DIR:-/mnt/us/signalk}"

CTL="/tmp/kundeploy-$$"
SSH_OPTS=(-p "$PORT" -o StrictHostKeyChecking=accept-new
          -o ControlMaster=auto -o "ControlPath=$CTL" -o ControlPersist=300)
remote() { ssh "${SSH_OPTS[@]}" "$USER@$HOST" "$@"; }
trap 'ssh "${SSH_OPTS[@]}" -O exit "$USER@$HOST" >/dev/null 2>&1 || true' EXIT

if ! remote "[ -f '$DIR/startpaper.sh' ]"; then
  echo "Nothing to remove: $DIR/startpaper.sh is not on $HOST."
  exit 0
fi

# 1. Give the stock UI back, the way the launcher itself does: a `disable` file
#    makes its next run stop the app and start the jobs it stopped.
echo "== 1. giving the stock UI back =="
remote "cd '$DIR' && touch disable && sh '$DIR/startpaper.sh'; sleep 4; \
  if pidof paperdisplay >/dev/null; then echo 'the app is still running'; else echo 'the app has stopped'; fi; \
  initctl status framework 2>/dev/null"

# 2. The crontab.
echo
echo "== 2. the launcher =="
if [ "$KEEP_SSH" = 1 ]; then
  echo "kept in the crontab, switched off by $DIR/disable: it now only keeps ssh running."
  echo "To switch the app back on: delete $DIR/disable (or run deploy.sh again)."
else
  remote "DIR='$DIR' sh '$DIR/install-cron.sh' remove && rm -f '$DIR/disable'"
fi

# 3. Files.
echo
echo "== 3. files =="
if [ "$PURGE" = 1 ]; then
  remote "rm -rf '$DIR' /var/tmp/paperdisplay /var/tmp/paperdisplay.png; echo 'deleted $DIR (settings and logs included)'"
else
  echo "left in $DIR (settings included): deploy.sh puts it all back. --purge deletes them."
fi

echo
echo "Done. The Kindle is running its own software again."
[ "$KEEP_SSH" = 1 ] || echo "ssh stays up until the next reboot; start it from KOReader (Network, SSH server) after that."
