#!/usr/bin/env bash
# compile-device.sh - compile ONE device (one hardware variant of it), as
# declared in devices.json, dispatching to whatever build mechanism its
# "type" needs.
#
# A device (helm, can_sim) can be built for several kinds of hardware:
# its "variants" block in devices.json names each one (e.g. can_sim has
# "s3zero"; a board on another chip adds its own entry with its own
# fqbn). The variant name is also the key the firmware looks itself up
# by in the OTA manifest, see the OTA section of the README.
#
# This is the single place that knows "how do I turn a device name into
# a compiled binary" - shared by compile-all.sh (build everything, no
# side effects) and release.sh. It has zero git/version-bump/publish
# side effects - it just compiles whatever's currently on disk.
#
# Prints the resulting binary's absolute path as the LAST line of
# stdout (everything else - build progress, docker output - goes to
# stderr) so callers can capture just the path with $(...).
#
# Usage: ./compile-device.sh <device>[:<variant>]
#        (variant may be left out when the device has only one)
#
# Requires: jq, and docker for "type": "arduino-cli" (via
# docker/compile.sh), or whatever a "type": "custom" device's own
# build_cmd requires.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "$script_dir/.." && pwd)"
devices_json="$script_dir/devices.json"

command -v jq >/dev/null 2>&1 || { echo "error: jq is required but not on PATH" >&2; exit 1; }

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <device>[:<variant>]" >&2
    exit 1
fi
device="${1%%:*}"
variant=""
[[ "$1" == *:* ]] && variant="${1#*:}"

jq -e --arg d "$device" 'has($d)' "$devices_json" >/dev/null || {
    echo "error: unknown device '$device' (not in $devices_json)" >&2; exit 1; }

# resolve the variant: given, or the only one the device has
if jq -e --arg d "$device" '.[$d] | has("variants")' "$devices_json" >/dev/null; then
    if [[ -z "$variant" ]]; then
        count="$(jq -r --arg d "$device" '.[$d].variants | length' "$devices_json")"
        if [[ "$count" -ne 1 ]]; then
            echo "error: '$device' has $count hardware variants ($(jq -r --arg d "$device" '.[$d].variants | keys | join(", ")' "$devices_json")) - say which: $device:<variant>" >&2
            exit 1
        fi
        variant="$(jq -r --arg d "$device" '.[$d].variants | keys[0]' "$devices_json")"
    fi
    jq -e --arg d "$device" --arg v "$variant" '.[$d].variants | has($v)' "$devices_json" >/dev/null || {
        echo "error: '$device' has no variant '$variant' (have: $(jq -r --arg d "$device" '.[$d].variants | keys | join(", ")' "$devices_json"))" >&2; exit 1; }
    # the device's own fields, with the variant's fields laid over them
    cfg="$(jq -c --arg d "$device" --arg v "$variant" '.[$d] + .[$d].variants[$v]' "$devices_json")"
else
    [[ -z "$variant" ]] || { echo "error: '$device' has no hardware variants" >&2; exit 1; }
    cfg="$(jq -c --arg d "$device" '.[$d]' "$devices_json")"
fi
label="$device${variant:+:$variant}"
type="$(jq -r '.type' <<<"$cfg")"

case "$type" in
    arduino-cli)
        sketch_ino="$(jq -r '.sketch_ino' <<<"$cfg")"
        fqbn="$(jq -r '.fqbn' <<<"$cfg")"
        sketch_rel_dir="$(dirname "$sketch_ino")"
        sketch_dir="$root_dir/$sketch_rel_dir"

        echo "[$label] compiling via Docker builder image ($fqbn)..." >&2
        "$script_dir/docker/compile.sh" "$sketch_rel_dir" "$fqbn" >&2

        # arduino-cli exports to build/<core-triplet>/<sketch>.ino.bin -
        # <core-triplet> is just the first 3 colon-separated fqbn
        # segments (board options after that don't appear in the path)
        # with ':' replaced by '.' - derived here instead of a separate
        # config field so it can never drift out of sync with the fqbn
        # actually used to compile.
        core_triplet="$(cut -d: -f1-3 <<<"$fqbn" | tr ':' '.')"
        sketch_name="$(basename "$sketch_ino" .ino)"
        bin_path="$sketch_dir/build/$core_triplet/$sketch_name.ino.bin"
        ;;
    custom)
        build_cmd="$(jq -r '.build_cmd' <<<"$cfg")"
        bin_path="$root_dir/$(jq -r '.bin_path' <<<"$cfg")"
        echo "[$label] compiling via custom build command: $build_cmd" >&2
        ( cd "$root_dir" && bash "$build_cmd" ) >&2
        ;;
    *)
        echo "error: unknown device type '$type' for '$label'" >&2
        exit 1
        ;;
esac

[[ -f "$bin_path" ]] || { echo "error: expected binary not found after compile: $bin_path" >&2; exit 1; }

echo "$bin_path"
