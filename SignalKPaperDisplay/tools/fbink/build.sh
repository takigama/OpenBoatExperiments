#!/usr/bin/env bash
# Builds FBInk for the Kindle PW3 family and writes dist/fbink/fbink-kindlepw2.
# Needs docker with BuildKit (any recent docker). Run from WSL.
set -euo pipefail
cd "$(dirname "$0")/../.."
docker build --target out --output type=local,dest=dist/fbink -f tools/fbink/Dockerfile tools/fbink
ls -l dist/fbink/fbink-kindlepw2
file dist/fbink/fbink-kindlepw2 | cut -c1-200
