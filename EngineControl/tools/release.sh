#!/usr/bin/env bash
# release.sh - build firmware for one or more devices/hardware variants,
# publish each image as a GitHub Release asset, and point ota/manifest.json
# at it. The firmware (HELM and the boards it updates over the bus) reads
# that manifest from raw.githubusercontent.com, finds its own hardware
# variant in it, and updates itself - same scheme as the other OpenBoat
# firmwares, with the extra "variant" level so each kind of board gets its
# own image:
#
#   ota/manifest.json
#   { "helm":    { "viewe7": { "build": 33, "url": "...", "md5": "..." } },
#     "can_sim": { "s3zero": { "build": 23, "url": "...", "md5": "..." } } }
#
# What it does, in order:
#   1. bumps the device's FW_BUILD (skip with --no-bump), once per device
#      even if several variants are released together
#   2. compiles each variant (tools/compile-device.sh, in Docker)
#   3. commits the bump and pushes it, so the release tag points at
#      source GitHub can see
#   4. creates one GitHub Release per variant: tag
#      ec-<device>-<variant>-b<build>, asset <device>-<variant>.bin
#   5. writes the new build/url/md5 into ota/manifest.json, commits and
#      pushes it - done last, so the manifest never points at an asset
#      that does not exist yet
#
# Tags and asset names are kept short on purpose: for a board updated over
# the bus the whole download URL has to fit in the CAN/ESP-NOW message that
# carries it (devices.json "url_max", 118 characters) alongside the WiFi
# credentials. The script refuses to release a URL that would not fit.
#
# Usage:
#   tools/release.sh [--dry-run] [--no-bump] <device>[:<variant>] ...
#     tools/release.sh can_sim                  # every can_sim variant
#     tools/release.sh helm can_sim:s3zero
#     tools/release.sh --dry-run can_sim        # build and show what would be
#                                               # released; touches no git/GitHub
#
# Requires: jq, gh (authenticated), git, md5sum, docker. Run it from a
# Linux shell (WSL) that has git push access to the repo.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "$script_dir/.." && pwd)"
devices_json="$script_dir/devices.json"
ota_dir="$root_dir/ota"

dry_run=0
bump=1
targets_arg=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) dry_run=1; shift ;;
        --no-bump) bump=0; shift ;;
        -h|--help) sed -n '/^# Usage:/,/^# Requires:/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "error: unknown option '$1' (see --help)" >&2; exit 1 ;;
        *) targets_arg+=("$1"); shift ;;
    esac
done
[[ ${#targets_arg[@]} -gt 0 ]] || { echo "error: say what to release, e.g. 'can_sim' or 'helm can_sim:s3zero' (see --help)" >&2; exit 1; }

for cmd in jq git md5sum docker; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "error: '$cmd' is required but not on PATH" >&2; exit 1; }
done
if [[ $dry_run -eq 0 ]]; then
    command -v gh >/dev/null 2>&1 || { echo "error: 'gh' is required but not on PATH" >&2; exit 1; }
    gh auth status >/dev/null 2>&1 || { echo "error: gh is not authenticated - run 'gh auth login' or set GH_TOKEN" >&2; exit 1; }
fi

# ---- the repo this lives in (EngineControl is a folder of the OpenBoat repo) ----
git_top="$(git -C "$root_dir" rev-parse --show-toplevel)"
rel="$(realpath --relative-to="$git_top" "$root_dir")"          # e.g. EngineControl
branch="$(git -C "$root_dir" rev-parse --abbrev-ref HEAD)"
origin_url="$(git -C "$root_dir" remote get-url origin)"
if [[ "$origin_url" =~ github\.com[:/]([^/]+/[^/.]+) ]]; then
    owner_repo="${BASH_REMATCH[1]}"
else
    echo "error: could not read owner/repo from the 'origin' remote: $origin_url" >&2; exit 1
fi
echo "Repo: $owner_repo  branch: $branch  folder: $rel"

# ---- expand the targets into device:variant pairs ----
all_targets() {
    jq -r 'to_entries[] | if .value | has("variants") then .key + ":" + (.value.variants | keys[]) else .key end' "$devices_json"
}
targets=()
for arg in "${targets_arg[@]}"; do
    if [[ "$arg" == *:* ]]; then
        all_targets | grep -qx "$arg" || { echo "error: unknown target '$arg' (have: $(all_targets | tr '\n' ' '))" >&2; exit 1; }
        targets+=("$arg")
    else
        mapfile -t found < <(all_targets | grep -E "^${arg}(:|$)" || true)
        [[ ${#found[@]} -gt 0 ]] || { echo "error: unknown device '$arg' (have: $(all_targets | tr '\n' ' '))" >&2; exit 1; }
        targets+=("${found[@]}")
    fi
done

# ---- preflight: a real release starts from a clean, pushed tree ----
if [[ $dry_run -eq 0 ]]; then
    dirty="$(git -C "$root_dir" status --porcelain -- . )"
    if [[ -n "$dirty" ]]; then
        echo "error: uncommitted changes in $rel - commit your firmware edits first:" >&2
        echo "$dirty" >&2
        exit 1
    fi
    git -C "$root_dir" fetch origin "$branch" >/dev/null
    if [[ "$(git -C "$root_dir" rev-parse HEAD)" != "$(git -C "$root_dir" rev-parse "origin/$branch")" ]]; then
        echo "error: local $branch differs from origin/$branch - push (or pull) first" >&2
        exit 1
    fi
fi

# ---- 1. bump FW_BUILD, once per device ----
declare -A build_of=()      # device -> build number
declare -A version_file_of=()
declare -A backup_of=()
for t in "${targets[@]}"; do
    d="${t%%:*}"
    [[ -n "${build_of[$d]:-}" ]] && continue
    vf="$root_dir/$(jq -r --arg d "$d" '.[$d].version_file' "$devices_json")"
    vdef="$(jq -r --arg d "$d" '.[$d].version_define' "$devices_json")"
    version_file_of[$d]="$vf"
    cur="$(grep -oP "#define\s+${vdef}\s+\K[0-9]+" "$vf")" || { echo "error: no '#define $vdef <N>' in $vf" >&2; exit 1; }
    if [[ $bump -eq 1 ]]; then
        backup_of[$d]="$(mktemp)"; cp "$vf" "${backup_of[$d]}"
        new=$((cur + 1))
        sed -i -E "s/(#define[[:space:]]+${vdef}[[:space:]]+)[0-9]+/\1${new}/" "$vf"
        echo "$d: build $cur -> $new"
        build_of[$d]="$new"
    else
        echo "$d: build $cur (not bumped)"
        build_of[$d]="$cur"
    fi
done

restore_bumps() {
    for d in "${!backup_of[@]}"; do cp "${backup_of[$d]}" "${version_file_of[$d]}"; done
}
if [[ $dry_run -eq 1 ]]; then trap restore_bumps EXIT; fi

# ---- 2. compile every target ----
mkdir -p "$ota_dir"
declare -A bin_of=() md5_of=() tag_of=() url_of=()
for t in "${targets[@]}"; do
    d="${t%%:*}"; v="${t#*:}"
    [[ "$t" == *:* ]] || v="default"
    build="${build_of[$d]}"
    src="$("$script_dir/compile-device.sh" "$t")"
    dest="$ota_dir/$d-$v.bin"
    cp -f "$src" "$dest"
    bin_of[$t]="$dest"
    md5_of[$t]="$(md5sum "$dest" | cut -d' ' -f1)"
    tag_of[$t]="ec-$d-$v-b$build"
    url_of[$t]="https://github.com/$owner_repo/releases/download/${tag_of[$t]}/$d-$v.bin"
    url_max="$(jq -r --arg d "$d" '.[$d].url_max // empty' "$devices_json")"
    if [[ -n "$url_max" && ${#url_of[$t]} -gt $url_max ]]; then
        echo "error: ${url_of[$t]} is ${#url_of[$t]} characters; boards updated over the bus can only be sent $url_max. Shorten the variant name or repo path." >&2
        exit 1
    fi
done

echo ""
echo "== ready =="
for t in "${targets[@]}"; do
    d="${t%%:*}"
    printf '%-18s build %-4s %s\n  tag %s\n  url %s (%d chars)\n  md5 %s\n' \
        "$t" "${build_of[$d]}" "$(du -h "${bin_of[$t]}" | cut -f1)" "${tag_of[$t]}" "${url_of[$t]}" "${#url_of[$t]}" "${md5_of[$t]}"
done

if [[ $dry_run -eq 1 ]]; then
    echo ""
    echo "[dry-run] nothing committed, pushed or released; FW_BUILD lines put back."
    exit 0
fi

# ---- 3. commit + push the bump ----
if [[ $bump -eq 1 ]]; then
    msg="EngineControl:"
    for d in "${!build_of[@]}"; do
        msg+=" $d build ${build_of[$d]},"
        git -C "$root_dir" add "${version_file_of[$d]}"
    done
    git -C "$root_dir" commit -m "${msg%,}" -- "${version_file_of[@]}"
    git -C "$root_dir" push origin "$branch"
fi
sha="$(git -C "$root_dir" rev-parse HEAD)"

# ---- 4. one GitHub Release per target ----
for t in "${targets[@]}"; do
    d="${t%%:*}"
    gh release create "${tag_of[$t]}" "${bin_of[$t]}" --repo "$owner_repo" --target "$sha" \
        --title "${tag_of[$t]}" --notes "$t build ${build_of[$d]} (md5 ${md5_of[$t]})"
done

# ---- 5. the manifest, last ----
manifest="$ota_dir/manifest.json"
json="{}"
[[ -f "$manifest" ]] && json="$(cat "$manifest")"
for t in "${targets[@]}"; do
    d="${t%%:*}"; v="${t#*:}"
    json="$(jq --arg d "$d" --arg v "$v" --argjson build "${build_of[$d]}" \
                --arg url "${url_of[$t]}" --arg md5 "${md5_of[$t]}" \
                '.[$d][$v] = {build: $build, url: $url, md5: $md5}' <<<"$json")"
done
echo "$json" > "$manifest"
git -C "$root_dir" add -f "$manifest"
git -C "$root_dir" commit -m "EngineControl: OTA manifest for ${targets[*]}" -- "$manifest"
git -C "$root_dir" push origin "$branch"

echo ""
echo "Released: ${targets[*]}"
echo "Manifest: https://raw.githubusercontent.com/$owner_repo/$branch/$rel/ota/manifest.json"
