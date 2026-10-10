# The one build image for every ESP32 firmware in this repo.
#
# Everything we build is PlatformIO + the Arduino framework (see build/README.md for why), so this image is
# just PlatformIO with the ESP32 platform and the toolchains for the three chip families we use already
# unpacked inside it:
#
#     esp32     (classic: the CYD displays)
#     esp32s3   (the 7in HELM, the S3-Zero)
#     esp32c3   (the C3 boards)
#
# Nothing is downloaded when a firmware is built except that project's own Arduino libraries (lib_deps), and
# those are cached on the host under build/.work. build/build.sh builds this image when it is missing or this
# file / the platform pin changed; you should not normally need to run docker by hand.
#
# Which platform release is used is written in ONE place - the `platform = ...` line of build/common.ini.
# build.sh reads it from there and hands it in as PLATFORM_URL, so the image and the projects can never
# disagree. To move to a newer one edit that line, rebuild with `build/build.sh --rebuild-image`, and build
# everything.

FROM python:3.12-slim-bookworm

# pioarduino = the PlatformIO platform for arduino-esp32 3.x (the stock "espressif32" platform stops at 2.x)
ARG PLATFORM_URL
# PlatformIO Core: the pioarduino platform needs 6.1.19 or newer
ARG PLATFORMIO_VERSION=6.1.19

RUN apt-get update && apt-get install -y --no-install-recommends \
        git ca-certificates jq \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir "platformio==${PLATFORMIO_VERSION}" esptool

# one world-writable PlatformIO home, so the container can run as the calling user (build.sh passes --user) and
# the files it writes into the repo belong to that user, not root
ENV PLATFORMIO_CORE_DIR=/opt/pio \
    PLATFORMIO_GLOBALLIB_DIR=/opt/pio/lib \
    PLATFORMIO_NO_ANALYTICS=true \
    PLATFORMIO_SETTING_ENABLE_TELEMETRY=no \
    PLATFORMIO_CHECK_PLATFORMS_INTERVAL=0 \
    PLATFORMIO_CHECK_LIBRARIES_INTERVAL=0 \
    PLATFORMIO_CHECK_PLATFORMIO_INTERVAL=0 \
    HOME=/tmp/home

# Prime the toolchains: one throwaway project per chip family, so every package (compiler, framework, esptool)
# is already in /opt/pio when a real firmware is built.
RUN set -eux; \
    mkdir -p /prime/src "$HOME"; cd /prime; \
    echo 'void setup(){} void loop(){}' > src/main.cpp; \
    printf '[env:esp32]\nplatform = %s\nboard = esp32dev\nframework = arduino\n[env:esp32s3]\nplatform = %s\nboard = esp32-s3-devkitc-1\nframework = arduino\n[env:esp32c3]\nplatform = %s\nboard = esp32-c3-devkitm-1\nframework = arduino\n' \
        "$PLATFORM_URL" "$PLATFORM_URL" "$PLATFORM_URL" > platformio.ini; \
    pio run; \
    cd /; rm -rf /prime; \
    chmod -R a+rwX /opt/pio; \
    mkdir -p /tmp/home && chmod 1777 /tmp/home

WORKDIR /work
CMD ["pio", "--version"]
