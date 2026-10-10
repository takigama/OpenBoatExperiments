#!/usr/bin/env bash
# release.sh - build firmware, publish each image as a GitHub Release asset, and point the OTA manifest at it.
#
#   build/release.sh [--dry-run] [--no-bump] <target>...
#     build/release.sh ESP32Seatalk
#     build/release.sh EngineControl                    # every EngineControl firmware that has OTA
#     build/release.sh EngineControl-Helm EngineControl-CanSim-C3
#     build/release.sh --dry-run FishFinderProBluetooth # build and show the plan; touches no git / GitHub
#
# A target is a firmware or project name from build/projects.json (see build.sh --list).
#
# What it does, in order:
#   1. bumps each firmware's version in projects.json (skip with --no-bump)
#   2. builds them (build.sh) -> build/firmware/TOBE-<name>-v<version>.bin
#   3. commits and pushes the bump, so the release tag points at source GitHub can see
#   4. creates one GitHub Release per firmware, holding that file
#   5. writes the new build / url / md5 into the OTA manifest, commits and pushes it - LAST, so the manifest
#      never points at a file that does not exist yet
#
# The manifest (served by raw.githubusercontent.com, read by the firmware's TobeOta):
#     one firmware:   { "build": 4, "url": "...", "md5": "..." }
#     several:        { "<device>": { "<variant>": { "build": ..., "url": ..., "md5": ... } } }
#
# Release tag: "tobe-<name>-v<version>", or "<tag_prefix>-<version>" if the registry gives the firmware a
# tag_prefix. Boards updated over a bus (EngineControl's can_sim) get the download URL inside a short message,
# so the registry's url_max limits its length; those firmware use a short tag_prefix, and this script refuses
# a URL that would not fit.
#
# Requires: jq, gh (authenticated), git, md5sum, docker. Run it from a Linux shell (WSL) with push access.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "$script_dir/.." && pwd)"
registry="$script_dir/projects.json"
out_dir="$script_dir/firmware"

die() { echo "error: $*" >&2; exit 1; }

dry_run=0; bump=1
targets_arg=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) dry_run=1; shift ;;
        --no-bump) bump=0; shift ;;
        -h|--help) sed -n '2,/^# Requires:/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) die "unknown option '$1' (see --help)" ;;
        *) targets_arg+=("$1"); shift ;;
    esac
done
[[ ${#targets_arg[@]} -gt 0 ]] || die "say what to release, e.g. 'ESP32Seatalk' (see --help)"

for cmd in jq git md5sum docker; do command -v "$cmd" >/dev/null 2>&1 || die "'$cmd' is required but not on PATH"; done
if [[ $dry_run -eq 0 ]]; then
    command -v gh >/dev/null 2>&1 || die "'gh' is required but not on PATH"
    gh auth status >/dev/null 2>&1 || die "gh is not authenticated - run 'gh auth login' or set GH_TOKEN"
fi

field() { jq -r --arg n "$1" --arg f "$2" '.firmware[$n][$f] // empty' "$registry"; }

# ---- resolve targets (firmware names, or a project = all of its firmware that can be released) ----
targets=()
add_target() { local t; for t in "${targets[@]:-}"; do [[ "$t" == "$1" ]] && return; done; targets+=("$1"); }
for arg in "${targets_arg[@]}"; do
    found=0
    while read -r n; do add_target "$n"; found=1; done < <(
        jq -r --arg a "${arg,,}" '.firmware | to_entries[] | select((.key|ascii_downcase) == $a) | .key' "$registry")
    if [[ $found -eq 0 ]]; then
        while read -r n; do
            [[ "$(jq -r --arg n "$n" '.firmware[$n].ota | type' "$registry")" == "object" ]] || { echo "(skipping $n: no OTA)"; continue; }
            add_target "$n"; found=1
        done < <(jq -r --arg a "${arg,,}" '.firmware | to_entries[] | select(((.value.project // .key)|ascii_downcase) == $a) | .key' "$registry")
    fi
    [[ $found -eq 1 ]] || die "unknown target '$arg' (build.sh --list shows them)"
done
for n in "${targets[@]}"; do
    [[ "$(jq -r --arg n "$n" '.firmware[$n].ota | type' "$registry")" == "object" ]] ||
        die "$n has no OTA (no \"ota\" in projects.json) - build it with build.sh and flash it by USB"
done

# ---- the repository ----
branch="$(git -C "$root_dir" rev-parse --abbrev-ref HEAD)"
repo="$(jq -r .repo "$registry")"
reg_branch="$(jq -r .branch "$registry")"
[[ "$branch" == "$reg_branch" ]] || die "on branch '$branch' but projects.json says releases come from '$reg_branch'"
echo "Repo: $repo  branch: $branch"

# ---- preflight: a real release starts from clean, pushed source (for what is being released) ----
if [[ $dry_run -eq 0 ]]; then
    paths=(build lib)
    for n in "${targets[@]}"; do paths+=("$(field "$n" dir)"); done
    dirty="$(git -C "$root_dir" status --porcelain -- "${paths[@]}")"
    if [[ -n "$dirty" ]]; then
        echo "error: uncommitted changes - commit your firmware edits first:" >&2
        echo "$dirty" >&2
        exit 1
    fi
    git -C "$root_dir" fetch origin "$branch" >/dev/null
    [[ "$(git -C "$root_dir" rev-parse HEAD)" == "$(git -C "$root_dir" rev-parse "origin/$branch")" ]] ||
        die "local $branch differs from origin/$branch - push (or pull) first"
fi

# ---- 1. bump versions ----
backup="$(mktemp)"; cp "$registry" "$backup"
restore() { cp "$backup" "$registry"; }
if [[ $dry_run -eq 1 ]]; then trap restore EXIT; fi
bump_json() { jq --indent 4 --arg n "$1" '.firmware[$n].version += 1' "$registry" > "$registry.tmp" && mv "$registry.tmp" "$registry"; }
for n in "${targets[@]}"; do
    cur="$(field "$n" version)"
    if [[ $bump -eq 1 ]]; then bump_json "$n"; echo "$n: version $cur -> $((cur + 1))"
    else echo "$n: version $cur (not bumped)"; fi
done

# ---- 2. build ----
"$script_dir/build.sh" "${targets[@]}" >&2

# ---- the plan: tag, URL, md5 per firmware ----
declare -A bin_of=() md5_of=() tag_of=() url_of=() ver_of=()
for n in "${targets[@]}"; do
    ver="$(field "$n" version)"; ver_of[$n]="$ver"
    asset="TOBE-$n-v$ver.bin"
    bin_of[$n]="$out_dir/$asset"
    md5_of[$n]="$(md5sum "${bin_of[$n]}" | cut -d' ' -f1)"
    prefix="$(field "$n" tag_prefix)"
    if [[ -n "$prefix" ]]; then tag_of[$n]="$prefix-$ver"; else tag_of[$n]="tobe-$n-v$ver"; fi
    url_of[$n]="https://github.com/$repo/releases/download/${tag_of[$n]}/$asset"
    url_max="$(field "$n" url_max)"
    if [[ -n "$url_max" && ${#url_of[$n]} -gt $url_max ]]; then
        die "${url_of[$n]} is ${#url_of[$n]} characters; boards updated over a bus can only be sent $url_max. Use a shorter tag_prefix in projects.json."
    fi
done

echo ""
echo "== ready =="
for n in "${targets[@]}"; do
    printf '%-30s v%-4s %s\n  tag %s\n  url %s (%d chars)\n  md5 %s\n' "$n" "${ver_of[$n]}" "$(du -h "${bin_of[$n]}" | cut -f1)" \
        "${tag_of[$n]}" "${url_of[$n]}" "${#url_of[$n]}" "${md5_of[$n]}"
done

if [[ $dry_run -eq 1 ]]; then
    echo ""
    echo "[dry-run] nothing committed, pushed or released; versions put back."
    exit 0
fi

# ---- 3. commit + push the bump ----
if [[ $bump -eq 1 ]]; then
    msg="Release:"; for n in "${targets[@]}"; do msg+=" $n v${ver_of[$n]},"; done
    git -C "$root_dir" add "$registry"
    git -C "$root_dir" commit -m "${msg%,}" -- "$registry"
    git -C "$root_dir" push origin "$branch"
fi
sha="$(git -C "$root_dir" rev-parse HEAD)"

# ---- 4. one GitHub Release per firmware ----
for n in "${targets[@]}"; do
    gh release create "${tag_of[$n]}" "${bin_of[$n]}" --repo "$repo" --target "$sha" \
        --title "TOBE $n v${ver_of[$n]}" --notes "TOBE $n, build ${ver_of[$n]} (md5 ${md5_of[$n]})"
done

# ---- 5. the manifests, last ----
declare -A manifests=()
for n in "${targets[@]}"; do
    m="$(field "$n" manifest)"; [[ -n "$m" ]] || m="$(field "$n" dir)/ota/manifest.json"
    mf="$root_dir/$m"; mkdir -p "$(dirname "$mf")"
    [[ -f "$mf" ]] || echo "{}" > "$mf"
    dev="$(jq -r --arg n "$n" '.firmware[$n].ota.device // empty' "$registry")"
    var="$(jq -r --arg n "$n" '.firmware[$n].ota.variant // empty' "$registry")"
    if [[ -n "$dev" ]]; then
        jq --indent 2 --arg d "$dev" --arg v "$var" --argjson b "${ver_of[$n]}" --arg u "${url_of[$n]}" --arg m "${md5_of[$n]}" \
            'if $v == "" then .[$d] = {build: $b, url: $u, md5: $m} else .[$d][$v] = {build: $b, url: $u, md5: $m} end' "$mf" > "$mf.tmp"
    else
        jq --indent 2 -n --argjson b "${ver_of[$n]}" --arg u "${url_of[$n]}" --arg m "${md5_of[$n]}" '{build: $b, url: $u, md5: $m}' > "$mf.tmp"
    fi
    mv "$mf.tmp" "$mf"
    manifests[$m]=1
done
for m in "${!manifests[@]}"; do git -C "$root_dir" add -f "$root_dir/$m"; done
git -C "$root_dir" commit -m "OTA manifest: ${targets[*]}" -- "${!manifests[@]}"
git -C "$root_dir" push origin "$branch"

echo ""
echo "Released: ${targets[*]}"
for m in "${!manifests[@]}"; do echo "Manifest: https://raw.githubusercontent.com/$repo/$branch/$m"; done
