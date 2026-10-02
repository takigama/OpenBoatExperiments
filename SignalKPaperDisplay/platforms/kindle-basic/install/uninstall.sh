#!/usr/bin/env bash
# Takes the app off the Kindle (8th generation) again and gives it back to the
# stock software. See platforms/kindle-pw3/install/uninstall.sh (or --help).
#
#   bash platforms/kindle-basic/install/uninstall.sh <host> [port] [options]
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
export DEPLOY_REQUIRE_HOST=1
exec bash "$HERE/../../kindle-pw3/install/uninstall.sh" "$@"
