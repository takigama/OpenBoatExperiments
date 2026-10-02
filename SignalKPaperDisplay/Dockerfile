# Reproducible build image for SignalKPaperDisplay - same pattern as the
# rest of this repo's firmware projects (build in Docker, nothing installed
# on the host). Pure-Go, CGO disabled, so one image cross-compiles every
# target in platforms/ with no per-device C toolchain.
#
# Normally driven through the Makefile:
#   make image        # build this image once
#   make test
#   make host         # native build, for running the PNG preview locally
#   make kindle-pw3   # device binary -> dist/kindle-pw3/paperdisplay
FROM golang:1.23-bookworm

WORKDIR /work
ENV GOFLAGS=-buildvcs=false \
    CGO_ENABLED=0
