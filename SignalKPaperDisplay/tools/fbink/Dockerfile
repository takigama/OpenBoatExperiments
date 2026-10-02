# Builds a FULL FBInk (with image support) for Kindle PW2-and-newer devices.
#
# KOReader's bundled `fbink` is a minimal build with image support compiled
# out - it only needs FBInk's screen-refresh calls - so it can't draw our
# frames. FBInk's GitHub releases are source-only, so we build it here with
# KOReader's prebuilt cross-toolchain (koxtoolchain), which targets the same
# old glibc/kernel the Kindle runs.
#
#   bash tools/fbink/build.sh        # -> dist/fbink/fbink-kindlepw2
#
# Other devices use another koxtoolchain target and FBInk make target (for
# Kobo: toolchain "kobo", `make kobo`) - add a sibling build when needed.

FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential ca-certificates curl xz-utils zstd pkg-config \
 && rm -rf /var/lib/apt/lists/*

ARG KOX_TAG=2026.08
RUN curl -fsSL "https://github.com/koreader/koxtoolchain/releases/download/${KOX_TAG}/kindlepw2.tar.zst" \
  | tar --zstd -xC /opt

ARG FBINK_TAG=v1.25.0
RUN mkdir /src \
 && curl -fsSL "https://github.com/NiLuJe/FBInk/releases/download/${FBINK_TAG}/FBInk-${FBINK_TAG}.tar.xz" \
  | tar -xJ -C /src --strip-components=1

ENV PATH=/opt/x-tools/arm-kindlepw2-linux-gnueabi/bin:${PATH}
WORKDIR /src
RUN make kindle CROSS_TC=arm-kindlepw2-linux-gnueabi \
 && ls -l Release/fbink

# Only the binary leaves the build (see build.sh).
FROM scratch AS out
COPY --from=build /src/Release/fbink /fbink-kindlepw2
