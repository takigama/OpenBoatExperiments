# tools

Build and release helpers for the firmware in `engine_display/` (HELM and the CYD
displays) and `can_sim/`. Builds run in Docker (`docker/`), so no local toolchain is needed.

## Devices and hardware variants

`devices.json` lists each device (`helm`, `can_sim`) and, under `variants`, each kind of
hardware it is built for, with that hardware's `fqbn`:

| device | variant | board |
|---|---|---|
| `helm` | `viewe7` | VIEWE UEDX80480070E-WB-A, ESP32-S3 7" panel |
| `can_sim` | `s3zero` | Waveshare ESP32-S3-Zero |

The variant name is also the key the firmware finds its own image by in the OTA manifest.

```
tools/compile-all.sh                 # build everything, no side effects
tools/compile-device.sh can_sim      # one device (name the variant if it has several: can_sim:s3zero)
```

## OTA: how an update reaches a board

Firmware images are **GitHub Release assets**; `ota/manifest.json` (in the repo, read from
`raw.githubusercontent.com`) says which build is current for each device and variant:

```json
{ "helm":    { "viewe7": { "build": 33, "url": "https://github.com/.../ec-helm-viewe7-b33/helm-viewe7.bin",   "md5": "..." } },
  "can_sim": { "s3zero": { "build": 23, "url": "https://github.com/.../ec-can_sim-s3zero-b23/can_sim-s3zero.bin", "md5": "..." } } }
```

- HELM checks the manifest about once a day (and on "Check for Updates"). It updates itself from its
  own `helm` entry. For a board on the bus it uses the entry for the hardware that board reports, so
  a board only ever gets an image built for it.
- Plain `http://` works too: put `#define OTA_MANIFEST_URL "..."` in a git-ignored
  `engine_display/local_config.h` (copy `local_config.example.h`) to try a manifest of your own.

## Releasing

```
tools/release.sh --dry-run can_sim   # build it and show the tag/url/md5; touches no git or GitHub
tools/release.sh can_sim             # every variant of can_sim
tools/release.sh helm can_sim:s3zero
```

Run it from WSL/Linux with `gh` logged in and the tree committed and pushed. It bumps `FW_BUILD`,
compiles, commits and pushes the bump, creates one release per variant
(`ec-<device>-<variant>-b<build>`, asset `<device>-<variant>.bin`), then writes and pushes the
manifest last. The download URL for a bus-updated board must fit the update message
(`url_max` in `devices.json`); the script checks.

## Adding a board

1. A `variants` entry in `devices.json` (name + `fqbn`).
2. For a `can_sim`-style board: an id in `can_protocol.h` (`HW_*`, `HW_COUNT`, `md_hw_key()`, both
   copies of the header) and a `HW_ID` for it in `can_sim.ino` (it stops with an `#error` on a chip
   it does not know). Pins differ per board, so check the pin block too.
3. `tools/release.sh <device>:<variant>`.
