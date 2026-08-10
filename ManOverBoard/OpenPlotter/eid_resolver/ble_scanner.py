"""
Direct BLE scanning using the Pi's own onboard Bluetooth radio (via bleak/
BlueZ), feeding the exact same brain.record_sighting() pipeline the HTTP
/sightings endpoint uses. This makes the controller a scanning node in its
own right, not just a passive sink waiting for a CYD or headless C3 to
report - useful standalone (no other hardware needed at all) and as one
more vantage point once other scanners are added.

Filtering mirrors the ESP32 firmware's FmdnAdvertisedDeviceCallbacks
exactly: Eddystone service UUID 0xFEAA, service-data frame type byte 0x40
or 0x41, 20-byte EID immediately following.
"""

from __future__ import annotations

import asyncio
import sys
import threading

from bleak import BleakScanner
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData

from eid_resolver.brain import Brain

FMDN_SERVICE_UUID = "0000feaa-0000-1000-8000-00805f9b34fb"
FRAME_TYPE_EID = (0x40, 0x41)
SOURCE_NAME = "pi-onboard"


def _extract_eid_hex(advertisement_data: AdvertisementData) -> str | None:
    data = advertisement_data.service_data.get(FMDN_SERVICE_UUID)
    if data is None or len(data) < 21:
        return None
    if data[0] not in FRAME_TYPE_EID:
        return None
    return data[1:21].hex()


def _make_callback(brain: Brain):
    def _on_detection(device: BLEDevice, advertisement_data: AdvertisementData) -> None:
        eid_hex = _extract_eid_hex(advertisement_data)
        if eid_hex is None:
            return
        brain.record_sighting(eid_hex, advertisement_data.rssi, source=SOURCE_NAME)

    return _on_detection


async def _scan_forever(brain: Brain) -> None:
    # bluez.filters.DuplicateData=True is NOT optional here: bleak's own
    # default is False (overriding BlueZ's own default of True - see
    # bleak.backends.bluezdbus.scanner.BlueZDiscoveryFilters' docstring),
    # meaning BlueZ deduplicates repeated advertisement payloads before
    # ever notifying bleak over D-Bus. Confirmed empirically on the real Pi
    # (2026-07-23) via a raw btmon HCI capture: the radio itself received
    # continuous advertisements (max gap 0.124s across 25s / ~1400 events),
    # but this scanner's own sightings showed gaps up to 8-12s - since our
    # tags broadcast the *same* 20-byte EID for their whole ~1024s rotation
    # window, BlueZ's default deduplication was silently dropping most
    # repeats before they ever reached this callback.
    scanner = BleakScanner(
        detection_callback=_make_callback(brain),
        bluez=dict(filters=dict(DuplicateData=True)),
    )
    await scanner.start()
    try:
        while True:
            await asyncio.sleep(3600)
    finally:
        await scanner.stop()


def _thread_main(brain: Brain) -> None:
    try:
        asyncio.run(_scan_forever(brain))
    except Exception as e:  # noqa: BLE001 - this is a best-effort scanning
        # source; if the Pi has no BT adapter, BlueZ isn't running, or the
        # process lacks permission, the HTTP API (and any remote scanners
        # posting to /sightings) should keep working regardless.
        sys.stderr.write(f"[ble-scanner] onboard BLE scan not available: {e}\n")


def start(brain: Brain) -> threading.Thread:
    """Starts the onboard BLE scan in a background thread with its own
    asyncio loop (bleak is async-only; the rest of this server is plain
    threading, same pattern as the existing check_missing background
    thread). Returns the thread so callers can decide whether to join it -
    the server just lets it run as a daemon."""
    t = threading.Thread(target=_thread_main, args=(brain,), daemon=True, name="ble-scanner")
    t.start()
    return t
