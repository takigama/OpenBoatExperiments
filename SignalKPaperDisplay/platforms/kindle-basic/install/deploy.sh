#!/usr/bin/env bash
# Copies the app to the Kindle Basic over SSH: this device's own profile and
# binary, with the launcher and the rest of the tooling shared with the
# Paperwhite 3. Run it yourself: ssh asks for the password once.
#
#   bash platforms/kindle-basic/install/deploy.sh <host> [port]
#
# See platforms/kindle-pw3/install/deploy.sh for how it works and what it
# honours (KINDLE_HOST, KINDLE_PORT, KINDLE_USER, KINDLE_DIR).
set -eu
# No default address: the shared script's default is the Paperwhite 3's, and this
# must never quietly deploy this device's profile to that one.
if [ $# -lt 1 ] && [ -z "${KINDLE_HOST:-}" ]; then
  echo "usage: bash platforms/kindle-basic/install/deploy.sh <host> [port]" >&2
  exit 2
fi
HERE="$(cd "$(dirname "$0")" && pwd)"
export DEPLOY_PLATFORM_DIR="$(dirname "$HERE")"
exec bash "$HERE/../../kindle-pw3/install/deploy.sh" "$@"
