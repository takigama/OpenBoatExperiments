# build/ - one way to build every ESP32 firmware

```
build/build.sh EngineControl          # all of one project's firmware
build/build.sh ESP32Seatalk           # one project
build/build.sh EngineControl-Helm     # one firmware
build/build.sh all                    # everything
build/build.sh --list                 # what there is, with versions
```

Results land in `build/firmware/`:

| file | what it is |
|---|---|
| `TOBE-<name>-v<version>.bin` | the application. What OTA installs; what you flash at `0x10000` to update a board and keep its settings. |
| `TOBE-<name>-v<version>-factory.bin` | bootloader + partitions + application in one file, for a board that never had firmware (flash at `0x0`). Erases saved settings. |

Everything is built **inside Docker** (nothing is installed on your machine). Needs `bash`, `docker`, `jq`, `md5sum`; run it from
Linux or WSL. The first run builds the image (~15 minutes, it unpacks the compilers for the three chip families); after that a
build is just a compile. Libraries a project downloads (`lib_deps`) are cached in `~/.cache/tobe-build` (override with `TOBE_CACHE`), outside the repository.

## Why PlatformIO

Five of our six ESP32 projects were already PlatformIO; only EngineControl used `arduino-cli`. PlatformIO won because shared code is
a first-class idea (`lib_extra_dirs`), a project's libraries are declared next to it (`lib_deps`) rather than baked into an image,
and one project can build for several boards by adding an `[env:...]` (EngineControl's `can_sim` is the C3 and the S3-Zero from one
source). The framework is still Arduino (arduino-esp32 3.3 on ESP-IDF 5.5, via the *pioarduino* platform), so all the code that was
Arduino stays Arduino.

## Files

| | |
|---|---|
| `Dockerfile` | the one build image: PlatformIO plus the toolchains for ESP32 / ESP32-S3 / ESP32-C3 |
| `common.ini` | settings every project shares: **the platform version pin**, the three chip bases, the CYD display pins |
| `projects.json` | the registry: every firmware, its project folder, PlatformIO environment, **version**, and where its OTA manifest is |
| `scripts/tobe_pio.py` | PlatformIO script every project uses: turns the registry entry into `FW_BUILD`, `TOBE_FW_NAME`, `TOBE_OTA_*` for the compile, and makes the factory image |
| `build.sh` | the driver |
| `release.sh` | build + publish to GitHub Releases + update the OTA manifest |

Nothing about a firmware's name, version or update location is written in its source: it comes from `projects.json`, so `build.sh`,
`pio run` run by hand, and `release.sh` always agree.

## Naming

Every firmware, release file, web page title and heading starts with **TOBE** (takigama open boat experiments):
`TOBE-EngineControl-CanSim-C3-v28.bin`, a page titled "TOBE EngineControl-Helm", the setup WiFi network `TOBE-<name>-A4CF`.
The name is the registry key; `tobe::title()` gives the display form.

## Adding a firmware

1. Make a PlatformIO project folder with a `platformio.ini` that pulls in `common.ini` and the shared libraries - copy the
   smallest existing one (`FishFinderProBluetooth/PlatformIO/FishFinderProBluetooth/platformio.ini`) and fix the relative paths.
2. Add an entry to `projects.json`.
3. `build/build.sh <name>`.

## Changing the toolchain

The platform release is the `platform =` line in `common.ini`, and only there (the Dockerfile reads it from there). Change it, run
`build/build.sh --rebuild-image`, then `build/build.sh all` to see what the new compiler thinks of everything.

## Releasing

`build/release.sh <name>...` - see the header of that script. A release is a GitHub Release per firmware holding the `.bin`, then
the OTA manifest updated last so it never points at a file that is not there yet.
