#!/usr/bin/env bash
# build.sh - build firmware. The only command you need.
#
#   build/build.sh <target>...         build these, put the firmware in build/firmware/
#   build/build.sh all                 build everything
#   build/build.sh --list              what can be built
#
# A target is a firmware name from projects.json (e.g. EngineControl-CanSim-C3) or a project name, which means
# all of that project's firmware (e.g. EngineControl). Case does not matter.
#
# What it does for you: builds the Docker image if it is missing or its inputs changed (Dockerfile, the platform pin in common.ini),
# runs PlatformIO inside it for each target, and copies the results to build/firmware/:
#
#   TOBE-<name>-v<version>.bin           the application: what OTA installs, and what you flash at 0x10000
#   TOBE-<name>-v<version>-factory.bin   bootloader + partitions + application in one file, for a board that has
#                                        never had firmware (flash at 0x0). It also erases saved settings, so do
#                                        NOT use it to update a board you want to keep configured.
#
# Options:
#   --list            show every target and its version, then stop
#   --clean           throw away the build cache of the targets first (a full rebuild)
#   --rebuild-image   rebuild the Docker image even if it looks current
#   --shell <target>  open a shell inside the build container, in that project's folder
#   -h, --help        this text
#
# Needs: bash, docker, jq, md5sum. Run it from Linux or WSL (the Windows side has no toolchain on purpose).
# Libraries a project depends on (lib_deps) are downloaded the first time and cached in ~/.cache/tobe-build (not in the repository).

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "$script_dir/.." && pwd)"
registry="$script_dir/projects.json"
out_dir="$script_dir/firmware"
# Build caches (compiled objects, downloaded libraries) live on the Linux filesystem, NOT in the repository:
# compiling on the Windows-mounted drive (/mnt/d under WSL) is several times slower. Override with TOBE_CACHE.
work_root="${TOBE_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/tobe-build}"

die() { echo "error: $*" >&2; exit 1; }

clean=0; rebuild_image=0; list=0; shell_mode=0
targets_arg=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --list) list=1; shift ;;
        --clean) clean=1; shift ;;
        --rebuild-image) rebuild_image=1; shift ;;
        --shell) shell_mode=1; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) die "unknown option '$1' (see --help)" ;;
        *) targets_arg+=("$1"); shift ;;
    esac
done

for cmd in jq md5sum; do command -v "$cmd" >/dev/null 2>&1 || die "'$cmd' is required but not on PATH"; done
[[ -f "$registry" ]] || die "missing $registry"

all_names() { jq -r '.firmware | keys[]' "$registry"; }
field() { jq -r --arg n "$1" --arg f "$2" '.firmware[$n][$f] // empty' "$registry"; }

if [[ $list -eq 1 ]]; then
    printf '%-30s %-26s %-9s %s\n' "FIRMWARE" "PROJECT" "VERSION" "FOLDER [env]"
    jq -r '.firmware | to_entries[] | [.key, (.value.project // .key), (.value.version|tostring), (.value.dir + " [" + .value.env + "]")] | @tsv' "$registry" |
        while IFS=$'\t' read -r n p v d; do printf '%-30s %-26s v%-8s %s\n' "$n" "$p" "$v" "$d"; done
    exit 0
fi

[[ ${#targets_arg[@]} -gt 0 ]] || die "say what to build, e.g. 'build/build.sh EngineControl' (see --list)"

# ---- resolve targets: firmware name, project name, or "all" (case-insensitive) ----
targets=()
add_target() { local t; for t in "${targets[@]:-}"; do [[ "$t" == "$1" ]] && return; done; targets+=("$1"); }
for arg in "${targets_arg[@]}"; do
    if [[ "${arg,,}" == "all" ]]; then
        while read -r n; do add_target "$n"; done < <(all_names)
        continue
    fi
    found=0
    while read -r n; do add_target "$n"; found=1; done < <(
        jq -r --arg a "${arg,,}" '.firmware | to_entries[] | select((.key|ascii_downcase) == $a) | .key' "$registry")
    if [[ $found -eq 0 ]]; then
        while read -r n; do add_target "$n"; found=1; done < <(
            jq -r --arg a "${arg,,}" '.firmware | to_entries[] | select(((.value.project // .key)|ascii_downcase) == $a) | .key' "$registry")
    fi
    [[ $found -eq 1 ]] || die "unknown target '$arg' (try --list)"
done

command -v docker >/dev/null 2>&1 || die "'docker' is required but not on PATH"

# ---- the build image: tagged with a hash of what goes into it, built only when that changes ----
# The platform release is pinned in common.ini (the one place); the image is built for exactly that.
platform_url="$(sed -n 's/^platform *= *//p' "$script_dir/common.ini" | head -1 | tr -d '[:space:]')"
[[ -n "$platform_url" ]] || die "no 'platform = ...' line in build/common.ini"
image_tag="tobe-build:$( (cat "$script_dir/Dockerfile"; echo "$platform_url") | md5sum | cut -c1-12)"
if [[ $rebuild_image -eq 1 ]] || ! docker image inspect "$image_tag" >/dev/null 2>&1; then
    echo "== building the build image $image_tag (several minutes the first time) =="
    docker build --build-arg "PLATFORM_URL=$platform_url" -t "$image_tag" "$script_dir" >&2
fi

# The container runs as root: the pioarduino platform sets file times inside its own package folder, which only
# the owner may do. Nothing is written into the repository (the workspace is the cache), and the cache is handed
# back to the calling user when PlatformIO finishes.
user_args=(-e PYTHONDONTWRITEBYTECODE=1 -e "HOST_UID=$(id -u)" -e "HOST_GID=$(id -g)")
run_in_container() {   # run_in_container <project dir> <workspace name> <command...>
    local dir="$1" ws="$2"; shift 2
    mkdir -p "$work_root/$ws"
    docker run --rm "${user_args[@]}" \
        -v "$root_dir:/work" -v "$work_root:/cache" -w "/work/$dir" \
        -e "PLATFORMIO_WORKSPACE_DIR=/cache/$ws" \
        "$image_tag" bash -c 'rc=0; "$@" || rc=$?; chown -R "$HOST_UID:$HOST_GID" "/cache/'"$ws"'"; exit $rc' _ "$@"
}

if [[ $shell_mode -eq 1 ]]; then
    [[ ${#targets[@]} -eq 1 ]] || die "--shell takes exactly one target"
    n="${targets[0]}"
    mkdir -p "$work_root/$n"
    exec docker run --rm -it "${user_args[@]}" -v "$root_dir:/work" -v "$work_root:/cache" -w "/work/$(field "$n" dir)" \
        -e "PLATFORMIO_WORKSPACE_DIR=/cache/$n" "$image_tag" bash
fi

mkdir -p "$out_dir"
built=()
for n in "${targets[@]}"; do
    dir="$(field "$n" dir)"; env="$(field "$n" env)"; ver="$(field "$n" version)"
    [[ -f "$root_dir/$dir/platformio.ini" ]] || die "$n: $dir/platformio.ini not found"
    echo ""
    echo "== $n (v$ver)  $dir [$env] =="
    if [[ $clean -eq 1 ]]; then rm -rf "$work_root/$n"; fi
    run_in_container "$dir" "$n" pio run -e "$env" >&2

    bdir="$work_root/$n/build/$env"
    [[ -f "$bdir/firmware.bin" ]] || die "$n: no firmware.bin after the build ($bdir)"
    base="TOBE-$n-v$ver"
    cp -f "$bdir/firmware.bin" "$out_dir/$base.bin"
    if [[ -f "$bdir/firmware-factory.bin" ]]; then
        cp -f "$bdir/firmware-factory.bin" "$out_dir/$base-factory.bin"
    fi
    built+=("$base.bin")
done

echo ""
echo "== built (in build/firmware/) =="
for f in "${built[@]}"; do
    printf '  %-52s %9s bytes  md5 %s\n' "$f" "$(stat -c %s "$out_dir/$f")" "$(md5sum "$out_dir/$f" | cut -d' ' -f1)"
done
