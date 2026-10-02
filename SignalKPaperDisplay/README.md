# SignalKPaperDisplay

An interactive e-ink display for [SignalK](https://signalk.org/) data
(speed, heading, depth, wind, and AIS targets by bearing relative to your
heading), meant to run on Kindle and Kobo readers mounted on a boat, fed
from an OpenPlotter/SignalK server.

It's a single static Go binary that connects straight to SignalK's
WebSocket, draws the screen itself, and (once added) reads the touchscreen
to switch pages. Because it's pure Go with no C dependencies, one Docker
build image cross-compiles every target.

## Status

Early. Working now: SignalK client + data model with staleness tracking, the
Nav page, PNG preview output, and drawing to a real Kindle Paperwhite 3 via
its built-in `eips` (skipping unchanged frames, with a periodic flashing
full refresh to clear ghosting), plus `platforms/kindle-pw3/install/deploy.sh`
to copy it onto the device. Not built yet: touch input and page switching,
the Wind and AIS pages, an FBInk driver (needed for Kobo), and the on-device
launcher script (start at boot, supervise, fall back to the stock UI).

## Layout

```
cmd/paperdisplay/      the one binary; device chosen at runtime by -profile
internal/signalk/      data model + WebSocket client (local-receive-time stamps)
internal/render/       grayscale drawing toolkit (rects, lines, antialiased text)
internal/pages/        screens: pure functions, snapshot -> canvas, no I/O
internal/display/      Display interface; PNG (preview) and Kindle eips drivers
internal/profile/      reads a platform's profile.json
internal/app/          render loop tying state, pages and display together
platforms/<name>/      one directory per device (see below)
Dockerfile, Makefile   reproducible Docker build, one make target per platform
```

Code under `internal/` never mentions a specific device. Everything that
differs per device lives in `platforms/<name>/`:

- `profile.json` - screen size, gray levels, touch device and protocol
- `build.env` - `GOOS`/`GOARCH`/`GOARM` for the cross-compile
- `README.md` - hardware notes and what's still unverified
- later: the launcher/install scripts for that device's stock OS

## Adding a device

1. `mkdir platforms/<name>` with `profile.json` and `build.env`.
2. `make <name>` - the Makefile discovers it automatically.
3. If the device needs a different way of drawing or reading touch, add an
   implementation behind the `Display` interface (or the input equivalent)
   and select it through the profile - don't branch on device names in pages.

## Building and trying it

```
make image        # once: build the Docker build image
make test
make host         # native binary for previews -> dist/host/paperdisplay
make kindle-pw3   # device binary -> dist/kindle-pw3/paperdisplay
```

Preview against a SignalK server (for example the fake-data one used during
development) without touching a device:

```
dist/host/paperdisplay -signalk localhost:3001 \
    -profile platforms/kindle-pw3/profile.json -once -out out/nav.png
```

## Design notes

- **Staleness is a first-class concept.** Every value carries the local time
  it arrived; anything older than 5 seconds renders as `--`, and a black
  "NO DATA" banner appears if the server link drops. A frozen screen must
  never look live.
- **Pages are pure.** A page only turns a snapshot into pixels, so each is
  checked as a PNG on a PC and runs unchanged on every device.
- **No per-device C toolchain.** FBInk, the one C dependency, is run as a
  prebuilt binary on the device rather than linked in.
