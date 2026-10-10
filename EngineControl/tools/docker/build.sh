#!/usr/bin/env bash
# build.sh - ensure the project's arduino-cli builder image exists and is
# current, building it only when it actually needs to be.
#
# The image is tagged with a short hash of Dockerfile + versions.json
# together. If neither has changed since the last successful build,
# `docker image inspect` already finds a match for that exact tag and
# this script does nothing else - no `docker build` invocation, no
# network calls, no re-running installs. If either changed (a version
# bump in versions.json, or an edit to the Dockerfile itself), the hash
# - and therefore the tag - is different, nothing matches, and this runs
# `docker build`, which itself only re-executes the actual core/lib
# install RUN step if versions.json's *content* changed (see the
# Dockerfile's own comment on why it's COPY'd in before that step) -
# every other layer, including the base image pull, is still reused from
# Docker's own cache regardless.
#
# Usage: ./build.sh          # ensure the image exists, print its tag
#        ./build.sh --force  # rebuild even if a matching tag already exists
#
# Prints the resolved image tag (enginecontrol-builder:<hash>) on stdout
# as its last line - callers (compile.sh) can capture that directly.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
image_name="enginecontrol-builder"

command -v docker >/dev/null 2>&1 || { echo "error: docker is required but not on PATH" >&2; exit 1; }
command -v jq >/dev/null 2>&1 || { echo "error: jq is required but not on PATH" >&2; exit 1; }

force=0
[[ "${1:-}" == "--force" ]] && force=1

hash="$(cat "$script_dir/Dockerfile" "$script_dir/versions.json" | sha256sum | cut -c1-12)"
tag="$image_name:$hash"

if [[ $force -eq 0 ]] && docker image inspect "$tag" >/dev/null 2>&1; then
    echo "Builder image up to date: $tag" >&2
else
    echo "Building builder image: $tag (Dockerfile/versions.json changed, or --force)" >&2
    base_image="$(jq -r '.base_image' "$script_dir/versions.json")"
    arduino_cli_version="$(jq -r '.arduino_cli_version' "$script_dir/versions.json")"
    docker build \
        --build-arg "BASE_IMAGE=$base_image" \
        --build-arg "ARDUINO_CLI_VERSION=$arduino_cli_version" \
        -t "$tag" \
        -f "$script_dir/Dockerfile" \
        "$script_dir"
fi

# floating alias so other tooling (compile.sh) doesn't need to know or
# recompute the hash itself - always points at whatever's current
docker tag "$tag" "$image_name:latest"

echo "$tag"
