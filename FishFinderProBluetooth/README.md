# FishFinderProBluetooth

ESP32 bridge for a cheap castable BLE sonar fish finder → Serial (later
possibly SignalK / NMEA 0183 DPT/MTW, once the frame format is decoded).
The ESP32 acts as a BLE **central**, connects to the fish finder's GATT
service, and republishes whatever it sends over its USB serial link - raw
hex first, structured values once the frame format is decoded. The real
deployment target is a wired link straight into an RPi running
OpenPlotter, not WiFi - see "Firmware" below for why WiFi and BLE aren't
used together at all.

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

## GATT layout (from nRF Connect)

- Service `0000fff0-0000-1000-8000-00805f9b34fb`
  - `FFF1` [Notify] - the data stream
  - `FFF2` [Write/Notify/Read] - probably the command channel (not yet
    explored - likely where range/sensitivity settings get written)
  - `FFF3` [Read] - unknown
- Also exposes Device Information (`0x180A`); strings not read yet.
- No pairing/bonding needed. Streaming starts as soon as notifications on
  FFF1 are enabled - no init command required.

## Frame format (observed - capture was dry/no bottom, so incomplete)

140 bytes per frame, delivered as 7×20-byte BLE notifications, ~4.5
frames/sec.

```
[0-1]     53 46            "SF" sync marker
[2-7]     00 x6
[8-19]    header fields (see below)
[20-134]  115 bytes        raw echo amplitude bins - spike at bin 0
                            (transmit bang/ringdown), then a smooth rise
                            ~8->18, no distinct bottom peak in the dry
                            capture
[135-139] AA 55 AA 55 AA   end marker
```

Parsing strategy: resync on the `SF` marker, validate total length 140 and
the trailer bytes. The first frame after subscribing may arrive truncated
(notifications were already mid-stream) - discard and resync rather than
trying to salvage it.

### Header bytes (speculative - need a wet capture to confirm)

| Offset | Observation |
|---|---|
| 8, 9, 11, 12, 14 | Constant (`06 03 .. 05 50 .. 55`) - possibly echoed settings |
| 10, 13 | Move together, always 9 apart (`66/57` → `72/63`, then stable) - possibly temperature or battery |
| 15 | ~190, near-constant |
| 16 | Varies 86-140 |
| 17-19 | Small and noisy - possibly fish/weed/strength fields |

15 and 16 are the leading candidates for depth / "no bottom" sentinel,
but this needs a real bottom echo to confirm against.

## Next steps

1. **Wet capture at a known depth** (pool/jetty - a bucket is under the
   0.8m minimum range) - correlate the bottom peak in the amplitude bins
   against the header bytes above.
2. **Android HCI snoop** of the Fish Helper Pro app while changing range
   and sensitivity, to capture what gets written to FFF2.
3. Read the Device Information (`0x180A`) strings.
4. **This firmware**: NimBLE + Serial - emit raw frames as hex over USB
   serial (see below), so 1 and 2 can happen against real logged data;
   write the decoder offline in Python against that log, then port it
   back into this firmware once trusted.

## Firmware

`PlatformIO/FishFinderProBluetooth/` - connects to the fish finder as a
BLE central, reassembles the 140-byte frames from FFF1's notifications,
and prints each complete frame as one line of space-separated hex to
Serial (same convention the MQTT raw topics use elsewhere in this repo)
for offline logging/analysis. No decoding on-device yet - that's step 4
above, deliberately sequenced after a real wet capture exists to decode
against rather than guessing at the header fields now.

Build/flash via the Dockerfile in that directory (same reproducible-image
pattern as ESP32Seatalk - see its header comment for the exact commands).

**WiFi and BLE are mutually exclusive, chosen once per boot.** The
ESP32-C3 (like every ESP32 with WiFi+BT) has a single radio shared
between the two - confirmed via isolation testing that they can't run
reliably at once: WiFi TX becomes unreliable the moment BLE is actively
streaming, even after sequencing BLE to only start after WiFi joins. This
also matches how the device will actually be deployed - wired via USB
into an RPi running OpenPlotter, not on WiFi at all in normal use - so
**BLE mode is the default/production path**: connects to the fish finder
and streams frames to Serial only, no WiFi involved. **WiFi mode** exists
purely for occasional admin (config, OTA updates) and is chosen via:

- A line typed into the serial monitor - `wifi` or `ble` - which saves the
  choice to NVS and reboots. This is the *only* way back into WiFi mode,
  since there's no web UI reachable once WiFi is off.
- The web UI's "Switch to BLE mode" button, from within WiFi mode.

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
