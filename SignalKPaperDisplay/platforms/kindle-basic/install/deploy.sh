#!/usr/bin/env bash
# Installs the app on the Kindle (8th generation) over SSH, start to finish:
# this device's own profile and binary, with the launcher and the rest of the
# tooling shared with the Paperwhite 3. Run it yourself.
#
#   bash platforms/kindle-basic/install/deploy.sh <host> [port] [options]
#
# See platforms/kindle-pw3/install/deploy.sh (or --help) for the options and for
# what it does. It has no default address: the shared script's default is the
# Paperwhite 3's, and this must never quietly deploy this device's profile there.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
export DEPLOY_PLATFORM_DIR="$(dirname "$HERE")"
export DEPLOY_REQUIRE_HOST=1
exec bash "$HERE/../../kindle-pw3/install/deploy.sh" "$@"
