# FishFinderProBluetooth

ESP32 bridge for a cheap castable BLE sonar fish finder → Serial, as NMEA
0183 (`$SDDPT` depth, `$YXMTW` water temperature) alongside raw hex for
the still-undecoded amplitude bins. The ESP32 acts as a BLE **central**,
connects to the fish finder's GATT service, decodes each frame, and
writes it straight out over its USB serial link. The real deployment
target is a wired link straight into an RPi running OpenPlotter, not
WiFi - see "Firmware" below for why WiFi and BLE aren't used together at
all.

## Device

- Sold as "Advwin Portable Wireless Fish Finder" (AU, ~$110) - a rebrand of
  a generic Chinese castable sonar, probably from the Erchang family.
- Specs: 125 kHz, 90° beam, 0.8-36 m range, 500 mAh, 2.4 GHz BLE.
- Advertises as **"Fish Helper Pro"**, MAC `D3:01:01:02:2F:FE` (looks like a
  random static address, not necessarily stable across power cycles/units).
- Prior art: [cdot/Ping](https://github.com/cdot/Ping) reverse-engineered a
  different Erchang unit (IS1678 module, already-processed depth/fish
  values). **This unit uses a different protocol** - Ping's decoder does
  not apply directly, though its BLE connection approach is a useful
  reference.

Couple of things that are interesting to note, if the device is on charge
or not in the water, the android app refuses to connect to the device but the
device itself is actually constantly sending data, not sure why they
hamstrung the app that way, but in any case I wanted to something I can maybe
hook up to openplotter and signalk as a useful source for depth stuff.

## GATT layout

- Service `0000fff0-0000-1000-8000-00805f9b34fb`
  - `FFF1` [Notify] - the data stream (what this firmware currently uses)
  - `FFF2` [Write] - the command channel - sensitivity/range (see
    "Command format" below), sent by this firmware on connect and
    whenever changed via the `sens`/`range` serial commands
  - `FFF3` [Indicate] - unknown, not yet explored
- Also exposes Device Information (`0x180A`); strings not read yet.
- No pairing/bonding needed. Streaming starts as soon as notifications on
  FFF1 are enabled - no init command required.

## Frame format

140 bytes per frame, delivered as 7×20-byte BLE notifications, ~4.5
frames/sec. 

```
[0-1]     53 46            "SF" sync marker
[2]       status bitfield  bit3=OutOfWater, bit6=ChangeFull, bit7=IsCharging
[3-4]     water depth      16-bit big-endian, raw tenths-of-a-foot
[5-6]     fish depth       16-bit big-endian, same units; 0 if no target
                            or fish depth >= water depth
[7]       fish size        raw
[8]       battery          0-6 (clamped)
[9-10]    water temp       16-bit big-endian, raw tenths-of-a-Fahrenheit-
                            degree (regardless of the app's display unit)
[11]      frequency        mode index
[12]      depth range      configured range setting
[13]      checksum         sum(bytes[0..12]) & 0xFF
[14]      55               fixed sentinel, always 0x55
[15-134]  120 bytes        raw echo amplitude bins - not decoded yet
[135-139] AA 55 AA 55 AA   trailer
```

Unit conversions (also straight from the app's source, its
`mainTimerTask()` display-formatting method):
- Depth: `meters = (raw / 10.0) * 0.3048` (feet -> meters, exact)
- Temperature: `celsius = (raw - 320) / 18.0` (i.e. `(raw/10 - 32) * 5/9`,
  the standard F->C formula applied to the tenths-scaled raw value)
- Both fields have a valid-range gate before trusting them at all
  (depth: `20 <= raw <= 2000`; temperature: `0 < raw <= 2000`) - outside
  that range the app itself shows "no reading" rather than a real value,
  and this firmware skips emitting the corresponding NMEA sentence for
  the same reason.

Parsing strategy: resync on the `SF` marker, validate total length 140,
the trailer bytes, and the header checksum before trusting any decoded
field. The first frame after subscribing may arrive truncated
(notifications were already mid-stream) - discard and resync rather than
trying to salvage it.

### Command format (FFF2)

`53 46 01 <NoiseFilter> <SensitivityHundred> 00 <Range> 00 <checksum> 55`
- `Range` is an **index** into a preset list, not a raw distance -
  confirmed by testing (sending index `5` made the device's echoed-back
  "device depth range" debug field settle to `90`): 0=auto,
  1=10ft, 2=20ft, 3=30ft, 4=60ft, 5=90ft, 6=120ft, 7=150ft, 8=200ft
- `checksum` = sum of the preceding bytes & 0xFF, same formula as the
  data-frame checksum

The vendor app re-sends this on every single received frame
(unconditionally, from its receive path). This firmware doesn't mirror
that - it sends once on connect and again only when `sens`/`range`
actually changes a value, since the device doesn't appear to need a
continuous re-sync. (An early version of this firmware appeared to stall
the connection hard whenever a write went out; that turned out to be a
local testing artifact - a serial terminal left in echoed mode was
feeding the device's own debug output back to it as garbled commands -
not a real cost of writing FFF2 while FFF1 is streaming.)

## Next steps

1. Wet-test the depth/temperature decode against a known depth beyond
   the 0.8m minimum range (a pool/jetty) to double-check the conversion
   at the far end of the range, not just the values already confirmed.
2. Decode the 120 raw amplitude bins (offsets 15-134) - the header/
   trailer/checksum/units are all confirmed now, but the bins themselves
   still need a real bottom echo (or further decompiled-source digging)
   to know what a "detected bottom" actually looks like in that data.
3. Read the Device Information (`0x180A`) strings.
4. Wet-test at ~15m to confirm the device stays accurate at that depth
   (well short of the claimed 36m max but far more than confirmed so
   far); the `range` index mapping itself is already confirmed (see
   "Command format" above).

## Firmware

`PlatformIO/FishFinderProBluetooth/` - connects to the fish finder as a
BLE central, reassembles the 140-byte frames from FFF1's notifications,
and for each complete, checksum-valid frame writes both a raw hex line
(space-separated bytes, same convention the MQTT raw topics use
elsewhere in this repo) and, when the corresponding field is in its
valid range, a real NMEA 0183 `$SDDPT` (depth) and/or `$YXMTW` (water
temperature) sentence with a correct checksum - straight over Serial, no
downstream parsing beyond standard NMEA needed. The amplitude bins
aren't decoded yet, so only the raw hex line carries them for now.

Build/flash via the Dockerfile in that directory (same reproducible-image
pattern as ESP32Seatalk - see its header comment for the exact commands).

**WiFi and BLE are mutually exclusive, chosen once per boot.** The
ESP32-C3 (like every ESP32 with WiFi+BT) has a single radio shared
between the two - confirmed via isolation testing that they can't run
reliably at once: WiFi TX becomes unreliable the moment BLE is actively
streaming, even after sequencing BLE to only start after WiFi joins. This
also matches how the device will actually be deployed - wired via USB
into an RPi running OpenPlotter, not on WiFi at all in normal use - so
**BLE mode is the default/production path**, no WiFi involved - but it
boots idle rather than auto-streaming, waiting for a `start` command.
**WiFi mode** exists purely for occasional admin (config, OTA updates).
All of these are typed into the serial monitor:

- `wifi` / `ble` - switch mode; saves the choice to NVS and reboots. This
  is the *only* way back into WiFi mode, since there's no web UI
  reachable once WiFi is off. (WiFi mode also has the web UI's own
  "Switch to BLE mode" button, since that direction doesn't need serial.)
- `start` / `stop` - within BLE mode only, pause/resume scanning and
  streaming live, no reboot needed.
- `sens <0-100>` / `range <0-8>` - within BLE mode only, sent to the
  device over FFF2 (see "Command format" above); confirmation printed to
  the debug log, and the decoded "device depth range" debug line reports
  what the device echoes back.

**WiFi config is entirely runtime, not compiled in** - same pattern as
ESP32Seatalk, and deliberately so: this project does GitHub-hosted OTA,
which means the compiled `.bin` itself gets published as a Release asset.
An earlier version of this firmware used a `secrets.h`/`no_secrets.h`
compile-time split (gitignoring the real one); that keeps credentials out
of *source control*, but they'd still ship in plaintext inside every
published binary, recoverable with a plain `strings firmware.bin` -
gitignoring the source doesn't protect the built artifact. So instead: no
saved WiFi credentials → WiFi mode boots a SoftAP (`FishFinder-XXXX`,
open, at `192.168.4.1`) serving a join form - pick a network, enter its
password, it saves to NVS and reboots to join. Nothing network-related is
baked into the firmware image at all, so the `.bin` is safe to publish.

Some ESP32-C3 modules (this project's original test boards included) also
have a marginal antenna match that causes reflections back into the PA at
full TX power, breaking WiFi entirely (RX/scanning stays fine, only TX
fails) even outside of the WiFi/BLE coexistence issue above -
`wifi_manager.cpp` caps TX power to work around it.

## Notes

- Data rate is ~630 B/s - trivial for an ESP32, no buffering concerns.
- BLE range from a sensor sitting at the waterline is limited - mount the
  ESP32 with a clear line of sight to the water.
- Other units sold under the same/similar names may use different
  firmware/framing - this is reverse-engineered from one specific unit,
  not a spec.
