"""
Forwards any board running one of the serial-bridge firmwares
(../../ESP32/src/serial_bridge.cpp for the CYD,
../../ESP32-C3/src/serial_bridge_c3.cpp for a headless C3) into this
controller's existing /sightings HTTP endpoint - lets a board act as a
scanning node over a plain USB cable, with no WiFi/network stack of its
own, same brain.py pipeline as ble_scanner.py's onboard-Bluetooth path
(see server.py's docstring on the hub-and-spoke design - this is just one
more scanner reporting in, tagged with its own `source`).

Both firmwares' serial output is deliberately just one line per sighting,
nothing else - "SEEN <40 hex chars> <signed rssi>" - so parsing here is a
plain regex, no protocol framing needed. Run one instance of this script
per board, each with its own --port and --source (see KNOWN_USB_SERIAL_IDS'
comment for why auto-detection only picks the *first* match and isn't
enough once more than one board is plugged in at a time)."""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.request
import urllib.error

import serial
import serial.tools.list_ports

SIGHTING_RE = re.compile(r"^SEEN ([0-9a-fA-F]{40}) (-?\d+)\s*$")
DEFAULT_SOURCE_NAME = "cyd-serial"
DEFAULT_BAUD = 115200
DEFAULT_CONTROLLER_URL = "http://localhost:8734"

# Reconnect pacing after the serial port disappears (board unplugged/reset) -
# a fixed short retry rather than growing backoff, since a MOB detection
# feed going quiet for longer than this on a real boat is exactly the kind
# of gap this whole project exists to avoid.
RECONNECT_DELAY_SECS = 2

# (VID, PID) of the USB-UART bridge chips the CYD boards use - matched
# instead of the human-readable description string, which varies by
# OS/driver (confirmed on the real Pi: pyserial reports the CH340 here as
# the generic "USB Serial", not anything containing "ch340" - vid/pid are
# populated correctly regardless of platform).
KNOWN_USB_SERIAL_IDS = {
    (0x1A86, 0x7523),  # CH340 (CYD)
    (0x10C4, 0xEA60),  # CP2102/CP2104 (CYD, some boards)
}
# Espressif's own USB VID - matched separately (any PID) since a C3's native
# USB-CDC port (see ../../ESP32-C3/src/serial_bridge_c3.cpp's comment on why
# it has no separate UART bridge chip at all) can enumerate under a few
# different PIDs depending on build config, unlike the CYD's fixed-PID
# external bridge chips above.
ESPRESSIF_USB_VID = 0x303A


def guess_port() -> str | None:
    """Picks the first USB-serial port that looks like one of these
    boards - a known CH340/CP210x bridge chip (KNOWN_USB_SERIAL_IDS) or
    Espressif's own native-USB VID (ESPRESSIF_USB_VID). Best-effort
    convenience only, and only useful when exactly one board is plugged in -
    pass --port explicitly once more than one is connected at a time (see
    module docstring), or if this picks the wrong device."""
    for port in serial.tools.list_ports.comports():
        if (port.vid, port.pid) in KNOWN_USB_SERIAL_IDS or port.vid == ESPRESSIF_USB_VID:
            return port.device
    return None


def post_sighting(controller_url: str, eid_hex: str, rssi: int, source: str) -> None:
    body = json.dumps({"eid": eid_hex, "rssi": rssi, "source": source}).encode("utf-8")
    req = urllib.request.Request(
        f"{controller_url}/sightings", data=body,
        headers={"Content-Type": "application/json"}, method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            resp.read()
    except (urllib.error.URLError, TimeoutError) as e:
        # Best-effort: a single dropped sighting isn't fatal (the whole
        # design already tolerates missed BLE packets - see
        # ../../ESP32/FINDINGS.md section 6), but keep going rather than
        # crashing the whole bridge over one bad HTTP round-trip.
        sys.stderr.write(f"[serial_bridge] couldn't reach controller: {e}\n")


def run(port: str | None, baud: int, controller_url: str, source: str) -> None:
    while True:
        actual_port = port or guess_port()
        if actual_port is None:
            sys.stderr.write("[serial_bridge] no known serial port found, retrying...\n")
            time.sleep(RECONNECT_DELAY_SECS)
            continue
        try:
            with serial.Serial(actual_port, baud, timeout=1) as ser:
                sys.stderr.write(f"[serial_bridge] connected on {actual_port} @ {baud} (source={source})\n")
                while True:
                    raw = ser.readline()
                    if not raw:
                        continue  # read timeout, not a disconnect - keep looping
                    line = raw.decode("utf-8", errors="replace").strip()
                    match = SIGHTING_RE.match(line)
                    if match is None:
                        continue  # e.g. the firmware's one-time "READY ..." banner
                    eid_hex, rssi = match.group(1).lower(), int(match.group(2))
                    post_sighting(controller_url, eid_hex, rssi, source)
        except serial.SerialException as e:
            sys.stderr.write(f"[serial_bridge] serial error ({e}), reconnecting...\n")
            time.sleep(RECONNECT_DELAY_SECS)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=None,
                         help="Serial device (e.g. /dev/ttyUSB0 or /dev/ttyACM0). Auto-detected if omitted "
                              "- but only safe when exactly one board is plugged in, see module docstring.")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--controller-url", default=DEFAULT_CONTROLLER_URL,
                         help="Base URL of the running eid_resolver.server instance.")
    parser.add_argument("--source", default=DEFAULT_SOURCE_NAME,
                         help="Tag recorded against every sighting from this board - must be unique per "
                              "board when running more than one bridge at once (e.g. cyd-serial, "
                              "esp32c3-serial), so /debug's per-source breakdown can tell them apart.")
    args = parser.parse_args()
    run(args.port, args.baud, args.controller_url, args.source)


if __name__ == "__main__":
    main()
