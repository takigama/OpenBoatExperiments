#!/usr/bin/env bash
# Takes the app off a Kindle again, over SSH, and gives it back to the stock
# software. Run it yourself: ssh asks for the password once (or uses your key).
#
#   bash platforms/kindle-pw3/install/uninstall.sh [host] [port] [options]
#   bash platforms/kindle-basic/install/uninstall.sh <host> [port] [options]
#
# It sends device-uninstall.sh (beside this one) to the Kindle and runs it there.
# That is the same script the installer leaves on the Kindle as
# /mnt/us/signalk/uninstall.sh, so you can also run it without a PC:
#
#   sh /mnt/us/signalk/uninstall.sh [options]
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

HOST_ARG=""; PORT_ARG=""; FLAGS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --keep-ssh|--purge) FLAGS="$FLAGS $1"; shift ;;
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
case "$FLAGS" in *--keep-ssh*--purge*|*--purge*--keep-ssh*)
  echo "--purge deletes the launcher, so it cannot be combined with --keep-ssh" >&2; exit 2 ;;
esac

HOST="${HOST_ARG:-${KINDLE_HOST:-10.0.0.164}}"
PORT="${PORT_ARG:-${KINDLE_PORT:-2223}}"
USER="${KINDLE_USER:-root}"
DIR="${KINDLE_DIR:-/mnt/us/signalk}"

HERE="$(cd "$(dirname "$0")" && pwd)"
exec ssh -p "$PORT" -o StrictHostKeyChecking=accept-new ${UNINSTALL_SSH_OPTS:-} "$USER@$HOST" \
  "DIR='$DIR' sh -s --$FLAGS" < "$HERE/device-uninstall.sh"
