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

Early, but running on a real Kindle Paperwhite 3. Working now:

- SignalK client + data model with staleness tracking
- Pages: a large rotating Compass and an eight-box Nav grid (SOG, heading,
  depth, COG, VMG, the closest AIS contact, wind speed and angle), selectable at runtime.
  VMG is velocity made good to the wind: boat speed (through the water if sent,
  else over the ground) times the cosine of the true wind angle, so it needs
  true wind direction and heading. COG shows "--" while barely moving.
  What each Nav box shows is chosen in Settings > Nav boxes (a paged picker),
  from: SOG, STW, COG, heading (true and magnetic), VMG to wind and to the
  waypoint, depth, water and air temperature, air pressure, humidity, apparent
  and true wind, waypoint bearing / distance / time-to-go / time at current
  speed / ETA / cross-track error, closest point of approach (CPA) and time to
  it (TCPA) for the ship that will pass closest, the closest AIS contact,
  rate of turn, pitch, roll, rudder angle, autopilot mode and target, the
  house battery (volts, charge, amps), the engine (RPM, temperature, oil
  pressure, fuel rate) and the fresh, grey and black water tanks. Values that
  live under an id (batteries, engines, tanks) show the first by name
- Waypoint data comes from SignalK's course paths (the v2 course API's
  `navigation.course.calcValues.*`, or `navigation.courseGreatCircle` /
  `courseRhumbline.nextPoint.*`). On the compass a hollow arrowhead on the
  rim, pointing outward, marks the bearing to the waypoint, with a "W" under
  it (just inside its base, turned with it, so it isn't mistaken for the wind
  pointer)
- The device's own battery is shown in the middle of the header: a battery
  that fills with the charge, the percentage, and a lightning bolt while it is
  on external power (read from the Linux power_supply class, else lipc)
- Per-metric unit settings (metric/imperial preset plus overrides)
- Drawing via FBInk (~0.9 s per frame on a Paperwhite 3, including our own
  page rendering; skips unchanged frames, rations partial refreshes, fast DU
  waveform for those, flashing full refresh periodically and on page
  change), or the Kindle's built-in `eips` as a fallback, plus PNG preview
  output for working on a PC
- A launcher that keeps it running, rolls back a bad update and restores the
  stock UI if it can't stay up (see "Running it on the Kindle")
- Touch: tap the left/right third (or swipe) to change page; tap the cog
  at the top left to open settings
- The compass page is also the dashboard: AIS contacts as diamonds on the rim
  (solid = closing, hollow = opening, bigger = nearer), the wind as two markers pointing in at the
  rim - the apparent wind an "A" (a solid head over two hollow legs) and the
  true wind a solid arrowhead, the true one drawn only when it is more than 10
  degrees from the apparent -, the nearest contact's
  name and distance top left, water temperature top right, apparent wind speed
  bottom left, fuel gauges bottom right. Each of these is drawn only once the
  server has sent that data, and shows "--" if it later goes stale
- Settings: the imperial/metric/nautical preset, a unit for every metric, and
  an "Invert colours" switch (white on black), and the SignalK server address
  (typed on an on-screen keypad; the app reconnects at once), saved to
  `settings.json` beside the app (tap the back arrow to leave). A server set
  there beats the launcher's `SIGNALK_HOST` / the `-signalk` flag, which is
  only the default until one is chosen
- A backlight setting (Settings > Backlight): a tap bar, minus/plus, off and
  max. The light is driven through the device's sysfs backlight, or failing
  that lipc, as described in the platform's `profile.json` (`frontlight`); the
  setting is hidden where there is none. The level is saved and re-applied
  when the app starts
- The log can never fill the device: only slow or failed refreshes are logged
  (unless `-verbose`), the app empties its log at `-log-max` (256 KB), repeated
  connection failures are logged once, and the launcher trims it as a backstop
- A clock in the header, drawn by the app, with a heartbeat dot beside it that
  blinks every second (redrawn as its own tiny region, so it costs almost
  nothing) - if it stops, the app or the panel has hung
- A header guard: the stock UI's status bar keeps running after the Kindle
  framework is stopped and paints its clock over our header each minute. Since
  an unchanged frame is never resent, that clock would stay put, so the header
  strip is repainted every 10 s and just after each minute starts
  (`-header-guard`, 0 to turn it off)
- Self-update from GitHub releases (see below)
- `platforms/kindle-pw3/install/deploy.sh` to copy a build onto the device

Not built yet: Wind and AIS pages, front-light sliders, a timezone setting and
an install/update-now button in settings, and direct framebuffer drawing with
dirty-region updates.

## Running it on the Kindle

The app is started and watched by `platforms/kindle-pw3/install/startpaper.sh`,
run from cron once a minute. It's idempotent: it never starts a second copy,
keeps the screen awake, stops the stock UI when it takes over, counts
crashes (3 in 10 minutes rolls back to `paperdisplay.prev`; 5 restores the
stock UI until the crashes age out), and `touch /mnt/us/signalk/disable`
stops the app and brings the stock UI back within a minute.

One-time setup:

1. `bash platforms/kindle-pw3/install/deploy.sh` - copies the app, `fbink`
   and the launcher to `/mnt/us/signalk/`.
2. On the Kindle: `cp launcher.conf.example launcher.conf`, and set
   `SIGNALK_HOST`.
3. Add one line to a script cron already runs each minute:
   `/mnt/us/signalk/startpaper.sh`.

FBInk comes from `bash tools/fbink/build.sh`: KOReader's bundled `fbink` has
image support compiled out, so a full build is made from source in Docker
with KOReader's prebuilt cross-toolchain. `bash scripts/test_launcher.sh`
tests the launcher against stand-ins for the Kindle's commands.

## Updating from GitHub

Devices update themselves from GitHub releases. A manifest in the repo
(`update/manifest.json`) lists, per platform, the newest version, its
download URL and SHA-256. On the device:

```
./paperdisplay -check-update     # report whether a newer release exists
./paperdisplay -update           # install it (then restart the app)
./paperdisplay ... -update-every 6h   # check periodically while running
```

The download is verified against the manifest's SHA-256 before anything on
disk is touched; the previous binary is kept as `paperdisplay.prev` for
rollback. Downloads go through `curl` on devices whose profile says
`"fetch": "curl"` (the Kindle), otherwise Go's own HTTP client.

To publish a release: bump `VERSION`, run `bash scripts/release.sh` from WSL
(builds every platform, creates the GitHub release, rewrites the manifest),
then commit and push `update/manifest.json`. Devices only see the new
version once the manifest is pushed.

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
