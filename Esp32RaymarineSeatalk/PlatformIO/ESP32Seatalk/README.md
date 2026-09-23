# ESP32Seatalk

A bridge board between a Raymarine SeaTalk1 instrument bus, an NMEA2000/CAN
network, MQTT, and SignalK. Built on an ESP32-C3 (`Mine9.0:ESP32_C3_OLED`
module - see the hardware project one directory up).

Everything received on **SeaTalk** or **CAN** is relayed to **MQTT** and
**SignalK** unconditionally, whenever they're connected. The reverse
direction - injecting data from MQTT/SignalK (or bridging directly between
SeaTalk and CAN) - is opt-in per object type, controlled from the web UI's
routing matrix. See [Routing matrix](#routing-matrix) below.

## Hardware

| Signal | GPIO | Notes |
|---|---|---|
| SeaTalk | GPIO4 | Bit-banged, open-drain, via a BSS138 level shifter. Single wire, TX and RX share it (self-echo is expected, not a bug). |
| CAN TX | GPIO7 | To header J4 pin 3 (`CAN_TX_LL`), for an external CAN transceiver module. |
| CAN RX | GPIO10 | To header J4 pin 4 (`CAN_RX_LL`). |

No CAN transceiver is on this board itself - J4 expects a plug-in
transceiver module, whose CANH/CANL then go out to connector J3.

## Building and flashing

Everything is built inside a pinned Docker image (`Dockerfile`) rather than
relying on a host toolchain:

```bash
docker build -t esp32seatalk-build .
docker run --rm -v "$(pwd):/work" esp32seatalk-build pio run
```

Flashing has two paths:

- **GitHub-hosted OTA** (canonical, used once the board is deployed):
  `ota/manifest.json` in this repo carries `{build, url, md5}` for the
  current release. The device checks it against its own `FW_BUILD`
  (`platformio.ini`) on boot and via the web UI's "Check for updates"
  button, downloads over HTTPS (`setInsecure()` - see `ota_manager.h` for
  why), and verifies the MD5 before flashing.
- **Direct upload** (fast iteration): the web UI's Firmware section has a
  file-upload form that POSTs a `.bin` straight to `/ota/upload` - no MD5
  check needed, since it's a trusted local LAN transfer. Still push a
  matching GitHub Release + manifest bump afterwards so OTA stays in sync.

## First boot / WiFi

No saved WiFi credentials → the board starts a SoftAP
(`ESP32Seatalk-XXXX`, open, `192.168.4.1`) and serves the same config page
described below, with a WiFi-join form. Once joined, all further
configuration happens at the board's regular LAN address.

## Web UI

Everything lives at `/` on the board's IP:

| Path | Purpose |
|---|---|
| `/` | Main config page - all sections below |
| `/wifi/save` (POST) | Save WiFi credentials, reboot to join |
| `/mqtt/save` (POST) | Save MQTT broker host/port/base topic |
| `/signalk/save` (POST) | Save SignalK server host/port |
| `/route/save` (POST) | Save the routing matrix |
| `/ota/check`, `/ota/apply` | GitHub-manifest OTA check/apply |
| `/ota/upload` (POST, multipart) | Direct firmware upload |
| `/demo/start-cycling`, `/demo/start-manual`, `/demo/stop` (POST/GET) | Demo mode |
| `/seatalk/test-lamp` | Cycles the lamp command (0x30) on/off - visible physical confirmation on a real instrument |
| `/seatalk/test-nav-data/start`, `/.../stop` | Continuous 1Hz synthetic wind/speed/depth send, for validating TX against real instruments |
| `/log` | Plain-text tail of the debug log ring buffer - the only diagnostic surface once the board is on a real SeaTalk bus (USB shares 3.3V with the bus, can't have both connected) |

**Demo mode** (`demo_mode.h`) has two mutually-exclusive modes, sharing one
per-object enable checkbox set:
- **Cycling**: a simulated vessel - rudder drives heading, heading+speed
  dead-reckon a position and set COG, depth/wind/water-temp ramp
  independently.
- **Manual**: fixed values you enter, held constant and re-sent every
  second until stopped.

## MQTT topic mapping

Base topic defaults to `esp32seatalk`, configurable from the web UI's MQTT
section. Every topic below is `{base}/...`.

### Decoded values (SeaTalk/CAN → MQTT, always on when connected)

One topic per object type, at the SignalK-equivalent dot-path with dots
replaced by slashes. Payload is a plain ASCII number, 4 decimal places
(e.g. `12.3450`), SI units throughout - same units SignalK itself uses, so
there's one conversion path, not two.

| Object | Topic | Units | SeaTalk command decoded |
|---|---|---|---|
| Depth below transducer | `environment/depth/belowTransducer` | m | `0x00` |
| Speed through water | `navigation/speedThroughWater` | m/s | `0x20` |
| Trip log | `navigation/trip/log` | m | `0x21` |
| Total log | `navigation/log` | m | `0x22` |
| Apparent wind angle | `environment/wind/angleApparent` | rad (−=port) | `0x10` |
| Apparent wind speed | `environment/wind/speedApparent` | m/s | `0x11` |
| Water temperature | `environment/water/temperature` | K | `0x23` |
| Latitude | `navigation/position/latitude` | decimal degrees (+N) | `0x50` |
| Longitude | `navigation/position/longitude` | decimal degrees (+E) | `0x51` |
| Speed over ground | `navigation/speedOverGround` | m/s | `0x52` |
| Course over ground | `navigation/courseOverGroundTrue` | rad | `0x53` |
| GNSS time (UTC) | `navigation/datetime/secondsSinceMidnight` | s since midnight | `0x54` |
| GNSS satellite count | `navigation/gnss/satellites` | count | `0x57` |
| Magnetic variation | `navigation/magneticVariation` | rad | `0x99` (raw SeaTalk sign convention - **not verified** against SignalK's own sign spec, see the comment in `seatalk_decode.cpp`) |

Two object types don't fit the one-path-per-value pattern and get special
handling:

| Object | Topic(s) | Notes |
|---|---|---|
| Heading + rudder | `navigation/headingMagnetic` (rad) and `steering/rudderAngle` (rad, +right/−left) | One `0x9C` datagram → two topics, published together |
| GNSS date | `navigation/datetime/date` | Payload is a **string** `"YYYY-MM-DD"`, not a number - decoded from `0x56` |

Position (lat/lon) stays as two independent topics here rather than one
combined value, unlike SignalK's own `navigation.position` object - simpler
for MQTT, where there's no natural way to publish a compound value to one
topic.

### Inbound commands (MQTT → SeaTalk and/or CAN, opt-in)

Publish to `{base}/set/{same path as above}` and the value gets picked up,
parsed, and passed to the [routing matrix](#routing-matrix) - it only
actually reaches SeaTalk or CAN if you've enabled that (source, dest,
object) combination in the web UI. A `set/` message for an object with no
matrix row (trip/total log, satellite count, magnetic variation alone) is
parsed and accepted but can never be routed anywhere - there's no encoder
and no checkbox for it.

Only 11 of the object types above have matrix rows (and therefore can
actually be injected): Depth, Speed through water, Apparent wind
angle/speed, Water temperature, Position, Course over ground, Speed over
ground, Heading+rudder, GNSS time, GNSS date.

The heading/rudder and date paths work the same inbound as outbound -
`set/navigation/headingMagnetic` and `set/steering/rudderAngle` combine
into one heading+rudder update once both are known, and
`set/navigation/datetime/date` expects the `"YYYY-MM-DD"` string, not a
number.

This topic tree is deliberately separate from the plain publish topics
above, so the device's own outbound publishes can never be picked back up
by its own subscription and re-processed as if they were external
commands.

### Raw hex passthrough (any bus, both directions)

A lower-level channel that bypasses all of the above - no decoding, no
SignalK-path mapping, no routing matrix. Useful for capturing/replaying
exact bytes, or working with commands nothing here decodes yet.

| Topic | Direction | Payload |
|---|---|---|
| `raw/seatalk` | out | Hex dump of every SeaTalk datagram received, decoded or not - lowercase, space-separated, zero-padded (e.g. `00 02 00 9b 00 `) |
| `raw/can` | out | Hex dump of every parsed N2K message: 4-byte big-endian PGN, then the message's data bytes |
| `raw/signalk` | out | Hex dump of every raw incoming WebSocket text frame (the full JSON delta, as bytes) |
| `raw/seatalk/send` | in | Hex bytes → sent as one SeaTalk datagram (first byte = command, rest = data) |
| `raw/can/send` | in | Hex bytes → first 4 bytes = big-endian PGN, rest = payload; sent as that N2K message (tNMEA2000 handles framing/fast-packet splitting) |
| `raw/signalk/send` | in | Hex bytes decoded to UTF-8 text, sent verbatim as a WS text frame - no JSON validation, true passthrough |

Hex parsing on the inbound side is lenient (spaces/case ignored); outbound
dumps always use the same lowercase-space-separated format shown above.

## SignalK integration

Connects to `ws://<host>:<port>/signalk/v1/stream?subscribe=self` -
pushes deltas under context `vessels.self`, source label `esp32seatalk`,
and also receives deltas for the vessel's own context. Every value in the
MQTT table above maps to the exact same SignalK path (dots instead of
slashes) - `SeatalkDecode::canonicalPath()` is the single table both
publishers read from.

Position combines into one `navigation.position` delta
(`{latitude, longitude}`) rather than two separate paths, matching
SignalK's own convention (Latitude/Longitude arrive as two independent
SeaTalk datagrams internally, cached until both are known).

Inbound deltas whose `source.label` is `esp32seatalk` are ignored - the
server rebroadcasts every delta, including our own, to every subscriber on
that context, so without this filter the device's own values would bounce
straight back in as if an external client had sent them.

**Anonymous/unauthenticated writes only.** No device-access-token support
yet for a security-enabled server (the common default) - see
`signalk_manager.h`.

## CAN / NMEA2000

Built on ttlappalainen's NMEA2000 library for PGN encode/decode, with a
custom driver (`n2k_twai_driver.h/.cpp`) on ESP-IDF's TWAI API - the
library's own `NMEA2000_esp32` companion only supports the classic ESP32's
CAN peripheral, not the C3's.

| Object(s) | PGN | Notes |
|---|---|---|
| Depth | 128267 (Water Depth) | |
| Speed through water | 128259 (Speed) | |
| Apparent wind angle/speed | 130306 (Wind Data) | Each half sent independently, matching the two-datagram cadence they arrive at from SeaTalk |
| Water temperature | 130310 (Environmental Parameters) | |
| Position | 129025 (Position Rapid Update) | Lat+lon cached and combined, same as SignalK |
| Speed/course over ground | 129026 (COG/SOG Rapid Update) | Each half sent independently |
| Heading + magnetic variation | 127250 (Vessel Heading) | Variation is cached and folded in here rather than sent as its own PGN 127258 (which would need a days-since-1970 timestamp) |
| Rudder | 127245 (Rudder) | |
| GNSS time + date | 126992 (System Time) | Cached and combined; needs a days-since-1970 date, computed via a standard integer algorithm (Hinnant's `days_from_civil`) |

Trip/total log and satellite count have no PGN mapped - deliberately
skipped (log needs a timestamp source; satellite count would need the
heavier GNSS DOP/Satellites-in-View PGN for little real value).

**Unverified against a real N2K bus** - there is no CAN transceiver or
NMEA2000 network available to test against, unlike every other part of
this project. What's actually been confirmed: it builds against the real
PGN API, the board boots cleanly with the TWAI driver installed and no
transceiver attached, and exercising the TX path with real data doesn't
hang or crash - CAN TX just fails fast (10ms timeout) with no bus present.
PGN correctness, address claiming, and behavior against real N2K devices
remain unconfirmed.

## Routing matrix

The web UI's Routing section is a table: one row per routable object type,
one column per configurable (source → destination) pair:

- MQTT → SeaTalk, SignalK → SeaTalk, CAN → SeaTalk
- SeaTalk → CAN, MQTT → CAN, SignalK → CAN

Every checkbox defaults **off** - injecting synthetic data onto a real
instrument network is an explicit choice, not a surprise default.
Persisted to NVS.

SeaTalk → MQTT/SignalK and CAN → MQTT/SignalK are **not** in this matrix -
those always relay, matching the project's original "unconditionally
relay everything received" design. MQTT ↔ SignalK and any bus to itself
are simply never a thing (not shown, not configurable).

`RouteConfig::relay(source, event)` (`route_config.h/.cpp`) is the single
dispatch point every source calls through - SeaTalk RX in `main.cpp`, N2K
RX, MQTT's `set/` handler, and SignalK's delta handler. It decides
everything downstream: MQTT/SignalK always get SeaTalk/CAN traffic, and
whatever the matrix allows gets encoded back out.

## Known limitations

- CAN/NMEA2000 (base bridging, routing-matrix legs involving CAN, and raw
  CAN passthrough) has never touched a real bus - see above.
- SignalK is anonymous-write only; a security-enabled server needs the
  device-access-request token flow, not yet built.
- Magnetic variation's sign convention (SeaTalk `0x99`) hasn't been
  cross-checked against SignalK's actual sign spec.
- Trip log, total log, and satellite count are decode-only - no SeaTalk or
  CAN encoder, so they can never appear as routing-matrix rows or be
  injected via `set/`.
