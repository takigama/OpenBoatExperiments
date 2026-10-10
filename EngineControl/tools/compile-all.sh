#!/usr/bin/env bash
# compile-all.sh - compile every hardware variant of every device in
# devices.json, one after another. Pure build verification: no git, no
# version bumps, no release - safe to run any time by anyone who's
# cloned this repo.
#
# Usage: ./compile-all.sh                  # every device, every variant
#        ./compile-all.sh helm can_sim     # just these devices (all their variants)
#        ./compile-all.sh can_sim:s3zero   # just one variant

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
devices_json="$script_dir/devices.json"

command -v jq >/dev/null 2>&1 || { echo "error: jq is required but not on PATH" >&2; exit 1; }

# every "device:variant" (or bare "device" for one with no variants)
all_targets() {
    jq -r 'to_entries[] | if .value | has("variants") then .key + ":" + (.value.variants | keys[]) else .key end' "$devices_json"
}

targets=()
if [[ $# -gt 0 ]]; then
    for arg in "$@"; do
        if [[ "$arg" == *:* ]]; then
            targets+=("$arg")
        else
            mapfile -t found < <(all_targets | grep -E "^${arg}(:|$)" || true)
            [[ ${#found[@]} -gt 0 ]] || { echo "error: unknown device '$arg' (not in $devices_json)" >&2; exit 1; }
            targets+=("${found[@]}")
        fi
    done
else
    mapfile -t targets < <(all_targets)
fi

echo "To compile: ${targets[*]}"

for target in "${targets[@]}"; do
    echo ""
    echo "== $target =="
    bin_path="$("$script_dir/compile-device.sh" "$target")"
    echo "[$target] built: $bin_path"
done

echo ""
echo "All compiled successfully."
