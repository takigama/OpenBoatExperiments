#!/usr/bin/env bash
# compile.sh - compile a sketch inside the project's arduino-cli builder
# container instead of a locally-installed toolchain.
#
# Ensures the builder image is current (build.sh - a no-op if nothing
# changed, see that script's own comment), then runs arduino-cli compile
# inside it with the repo mounted at /workspace, so --export-binaries
# writes its output straight back to the host at the usual
# <sketch-dir>/build/<fqbn>/... path, same as a local arduino-cli would.
#
# Usage:
#   ./compile.sh <sketch-dir-relative-to-repo-root> <fqbn> [extra arduino-cli compile args...]
#
# Example:
#   ./compile.sh engine_display \
#       esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "$script_dir/../.." && pwd)"

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <sketch-dir> <fqbn> [extra arduino-cli compile args...]" >&2
    exit 1
fi
sketch_dir="$1"; fqbn="$2"; shift 2

image_tag="$("$script_dir/build.sh" | tail -1)"

docker run --rm \
    -v "$root_dir:/workspace" \
    -w /workspace \
    "$image_tag" \
    arduino-cli compile --fqbn "$fqbn" --export-binaries "$sketch_dir" "$@"
