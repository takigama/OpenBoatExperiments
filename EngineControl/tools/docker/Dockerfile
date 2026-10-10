# Builder image for this project's firmware: arduino-cli + every board
# core/library this project needs, all baked into the image at BUILD time
# (not installed fresh on every `docker run`) - see build.sh for the
# wrapper that decides when a (re)build is actually needed at all.
#
# versions.json is the single source of truth for what gets installed
# below - the base OS image, the exact arduino-cli release, and every
# core/library version. It's COPY'd in before the install RUN step
# specifically so Docker's own layer cache does the invalidation work for
# us: as long as versions.json (and this Dockerfile) haven't changed,
# `docker build` reuses the cached install layer instantly instead of
# re-running any installs at all: the real "did anything change" check.
# build.sh takes this one step further by tagging the resulting image
# with a hash of both files, so it can skip invoking `docker build`
# entirely when nothing changed, not just skip the expensive layer
# inside it.
#
# There is NO official/reliable pre-built "arduino-cli" base image on
# Docker Hub - confirmed the hard way (docker.io/arduino/arduino-cli
# doesn't exist; verified directly against Docker Hub's own API, not
# just a failed pull). arduino-cli itself is installed here via its own
# officially-documented install script instead, on top of a plain,
# definitely-real base (debian:bookworm-slim) - more reliable than
# depending on any third-party image's own versioning/maintenance, and
# in keeping with this project's habit of pinning every version
# explicitly rather than trusting an unpinned "latest" from someone else.
#
# BASE_IMAGE and ARDUINO_CLI_VERSION are both build ARGs (not written
# directly into this file) specifically so versions.json can change
# either without editing this file - build.sh reads them out of
# versions.json and passes them through via --build-arg.
ARG BASE_IMAGE=debian:bookworm-slim
FROM ${BASE_IMAGE}

ARG ARDUINO_CLI_VERSION=v1.5.1

RUN apt-get update && apt-get install -y --no-install-recommends \
        curl ca-certificates jq python3 \
    && rm -rf /var/lib/apt/lists/*

# official install method - see https://github.com/arduino/arduino-cli's
# own install.sh; BINDIR controls where the binary lands, the version
# tag is passed as the script's first positional argument (must include
# the leading "v", matching the GitHub release tag exactly)
RUN curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh \
    | BINDIR=/usr/local/bin sh -s "${ARDUINO_CLI_VERSION}"

COPY versions.json /tmp/versions.json

RUN set -eux; \
    urls="$(jq -r '.board_manager_urls | join(",")' /tmp/versions.json)"; \
    arduino-cli config init --additional-urls "$urls"; \
    arduino-cli core update-index; \
    jq -r '.cores[]' /tmp/versions.json | while read -r core; do \
        arduino-cli core install "$core"; \
    done; \
    jq -r '.libraries[]' /tmp/versions.json | while read -r lib; do \
        arduino-cli lib install "$lib"; \
    done

WORKDIR /workspace
