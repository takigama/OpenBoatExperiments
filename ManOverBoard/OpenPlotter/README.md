# MOB EID resolver

Off-box crypto resolver for the ManOverBoard project - the "bigger machine,
bridged over MQTT" piece mentioned (but never designed) since the ESP32
firmware's on-device crypto was abandoned (~2.1s/candidate was too slow -
see `../ESP32/FINDINGS.md` section 4). Runs the same validated
`generate_eid()` math as the ESP32/`drift_test.py`, but with a full CPU and
no BLE/WiFi radio to share it with.

## What it does

Holds the real EIKs for your tags, and acts as the central "controller" in
a hub-and-spoke design: any number of independent scanners - this host's
own onboard Bluetooth (`eid_resolver/ble_scanner.py`), a CYD or ESP32-C3
wired in over USB serial (`eid_resolver/serial_bridge.py` + the ESP32-side
`serial_bridge`/`serial_bridge_c3` firmware), or eventually a phone -
report raw sightings to `/sightings`. `brain.py` owns the actual
missing-tag/alarm decision and known-tag identification (cryptographic,
via `candidates.py`, not the ESP32's timing-heuristic swap logic - see
`../ESP32/src/display_mob_test.cpp`'s `find_ignore_swap_candidate()`).
A small dashboard (`/`), a per-scanner reception-quality breakdown
(`/debug`), and a settings page (`/settings`) are all served over plain
HTTP - see "Running it" below.

**This service never needs to run on the boat's own network to be
useful during development** - it's a standalone Python service, testable
anywhere.

## Status

- **Core crypto + candidate generation + HTTP API: built and tested.**
- **Deployed and running on a real Raspberry Pi (OpenPlotter) on the
  user's LAN** - not just a local dev loop. Confirmed working with real
  physical tags, real BLE hardware, over multi-hour unattended runs.
- **Known-tag identification: works, but needed two real bugs fixed
  first** (2026-07-22/23) - worth knowing about if this ever silently
  stops matching again:
  - The tag's internal EID-rotation clock is **not real UTC** (see
    `../ESP32/FINDINGS.md` section 5) - `candidates.py` originally had no
    concept of this at all, so known-tag matching never worked even with
    the correct EIK. Fixed by adding `device_time_offset_secs` (and an
    optional `reset_baseline_device_time` for instant recognition right
    after a battery pull) to each `known_tags.json` entry - see "Setup"
    below for the current field list.
  - `brain.py`'s per-sighting matching used to rebuild a full 24h-forward
    candidate table on *every single BLE sighting* (every ambient device
    nearby, not just known tags) - pegged a real Pi's CPU at 99.9% and
    made it fall permanently behind real time, which looked exactly like
    "stopped matching" from the dashboard. Fixed with `KnownTagCache` in
    `candidates.py` - a small per-tag EID cache refreshed only once per
    ~17-minute rotation, not per sighting. **If known-tag matching ever
    silently stops working again, check `ps aux`/CPU load on the host
    before assuming the crypto/keys are wrong.**
- **Multi-scanner reception tested extensively, with real numbers**: a
  single scanner (this host's onboard Bluetooth) alone saw worst-case
  gaps of 12-20s between sightings of a given tag. Adding a second
  scanner (a CYD over serial) brought the *combined* worst-case gap
  (whichever scanner catches a beacon first resets the clock) down to a
  reliable 6.0s; a third (an ESP32-C3, also over serial) brought it down
  further to 4-6s, confirmed over both short (~30-45 min) and long
  (6-hour) runs. Confirms the project's original "multi-receiver
  consensus matters more than any single threshold" design principle
  (`../ESP32/FINDINGS.md` section 6) empirically, not just in theory.
- **Real EIK extraction from a Google account: not built.**
  [GoogleFindMyTools](https://github.com/leonboe1/GoogleFindMyTools) already
  solves this - see `../ESP32/FINDINGS.md` section 1.2 for the process
  already used to populate the ESP32 side's `secrets.h`. This project
  expects EIKs to already be extracted and pasted into `known_tags.json`
  (see below), the same manual-key-file split the ESP32 firmware uses
  (`secrets.h`/`no_secrets.h`) - reimplementing the account-linking/OAuth
  flow itself is out of scope here.
- **OpenPlotter app packaging (Debian package, Settings-panel GUI
  integration): not built.** Runs as a plain standalone service (systemd
  unit or a plain terminal + `nohup`) - not yet wrapped as an
  `openplotter-settings`-integrated `.deb`.

## Setup

```
pip install -r requirements.txt
cp eid_resolver/known_tags.example.json eid_resolver/known_tags.json
```

Edit `known_tags.json` - each entry is:

```json
{
  "name": "Kyuubi",
  "eik_hex": "<64-char hex, the tag's real 32-byte EIK>",
  "device_time_offset_secs": 0,
  "reset_baseline_device_time": null
}
```

- `eik_hex`: extracted via GoogleFindMyTools - see `../ESP32/FINDINGS.md`
  section 1.2.
- `device_time_offset_secs`: `real_time - device_time` for this specific
  tag - discover it once via a brute-force search against a live sighting
  (see `candidates.py`'s module docstring for why this needs a small
  `OFFSET_TOLERANCE_WINDOWS` band, not an exact point value).
- `reset_baseline_device_time` (optional): the tag's factory
  reset-baseline epoch, if known (see `../ESP32/FINDINGS.md` section 5.1's
  table for this project's real tags) - lets a freshly-reset tag be
  recognized instantly instead of needing offset rediscovery.

`known_tags.json` is gitignored - never commit it, same as the ESP32
side's `secrets.h`.

## Running it

```
python -m eid_resolver.server
```

Starts the HTTP controller (default port 8734) and the onboard-Bluetooth
scanner (`ble_scanner.py`) together. Routes:

- `/` - dashboard (tag status, enroll/silence controls)
- `/debug` - per-tag *and* per-scanner-source reception stats (min/avg/max
  gap between sightings) - the per-source breakdown is what actually lets
  you compare scanners against each other, since the combined view mixes
  every source together
- `/settings` - missing-tag/escalation timeout tuning
- `/candidates` - the raw precomputed candidate-EID table (JSON)
- `/api/tags` - full tag state (JSON)
- `/sightings` (POST) - where any scanner reports a raw `{eid, rssi,
  source}` sighting

### Adding more scanners

A CYD or ESP32-C3 running the ESP32-side `serial_bridge`/`serial_bridge_c3`
PlatformIO environment (see `../ESP32/src/serial_bridge.cpp` /
`../ESP32-C3/src/serial_bridge_c3.cpp`) prints one `SEEN <eid> <rssi>` line
per sighting over USB serial. Bridge it in with:

```
python -m eid_resolver.serial_bridge --port /dev/ttyUSB0 --source cyd-serial
```

Run one instance per board, each with its own `--port` and a unique
`--source` tag (auto-detection only picks the first matching port, so
it's not safe to rely on once more than one board is plugged in - see
`serial_bridge.py`'s module docstring). The CYD enumerates as
`/dev/ttyUSB0`-style (CH340/CP210x USB-UART bridge); the C3 enumerates as
`/dev/ttyACM0`-style (native USB-CDC, no separate bridge chip) - the
script's `guess_port()` recognizes both.

## Verifying the crypto port

```
python -m eid_resolver.crypto
```

should print `Self-check: PASS` - confirms this is computing the exact
same EIDs as the ESP32 firmware and `drift_test.py`, not a subtly
different reimplementation.
