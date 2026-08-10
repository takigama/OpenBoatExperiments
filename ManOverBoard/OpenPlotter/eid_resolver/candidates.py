"""
Builds the rolling candidate-EID window each known tag hands to the CYD hub
(see the "just a lookup" design discussed for the hub-and-spoke MOB
architecture - the hub matches a heard EID against these candidates
instead of guessing at rotation heuristically).

Why a plain contiguous window needs no separate "drift margin" padding:
generate_eid() is stateless per ROTATION_PERIOD window (see crypto.py), so
a tag's own clock drifting a few seconds/day (typical for an uncompensated
crystal) can only shift which window boundary its *next* broadcast falls
in by roughly one window at the extreme edge of a day's accumulated drift -
and that adjacent window's EID is already present somewhere in a
contiguous 24h-spanning list regardless. The redundancy of covering a full
day already absorbs realistic drift; no extra candidates per generation
are needed on top of that.

Device-time offset (see ../ESP32/FINDINGS.md section 5): a tag's internal
EID-rotation clock is NOT real UTC - it free-runs from a fixed
factory-baked reference that only changes on a battery pull/reset. Each
known tag's `device_time_offset_secs` is `real_time - device_time`,
discovered once (originally on the ESP32) and cached here; candidates are
generated in device-time space (`real_ts - offset`) rather than assuming
the tag's clock matches ours. A tag with no offset recorded yet (offset 0)
simply won't match anything real until one is discovered via a wide
brute-force search, same technique as the ESP32's original discovery.

The discovered offset is only accurate to within about one rotation
window, not to the exact second - it's reconstructed from *when a sighting
was recorded* (scan + HTTP-post latency, seconds of slop) rather than the
tag's true internal window boundary, so two honest measurements of the
same physical tag a few minutes apart can disagree by +-1 window even
though the real underlying offset hasn't moved at all (confirmed
empirically on 2026-07-22 - re-deriving the same tag's offset twice, ~20
minutes apart, landed one ROTATION_PERIOD apart both times). The ESP32
firmware handles this with `WIDE_SEARCH_WINDOWS = +-5` search-widening on
drift (see full_featured.cpp); OFFSET_TOLERANCE_WINDOWS below mirrors that
same tolerance here rather than treating known offsets as exact.
"""

import json
import time
from pathlib import Path

from eid_resolver.crypto import ROTATION_PERIOD, generate_eid, mask_timestamp

DEFAULT_WINDOW_HOURS = 24
# NOTE: kept smaller than the ESP32's +-5 window WIDE_SEARCH_WINDOWS on purpose -
# this project's 3 known tags have device-time offsets only 1-2 windows apart
# from each other (see FINDINGS.md 5.1's "within a 23-minute window" finding),
# so a +-5 window band would make their candidate sets overlap and the match
# would become ambiguous between tags, not just tolerant of measurement jitter.
OFFSET_TOLERANCE_WINDOWS = 2


def compute_candidates(eik: bytes, start_ts: int, hours: int = DEFAULT_WINDOW_HOURS,
                        device_time_offset_secs: int = 0,
                        offset_tolerance_windows: int = OFFSET_TOLERANCE_WINDOWS) -> list[str]:
    """Returns hex-encoded EIDs for every rotation window from start_ts's
    window through `hours` hours forward, inclusive of the starting window
    so a tag doesn't briefly fall out of coverage right at a refresh
    boundary, AND for a small band of nearby offsets (+-offset_tolerance_
    windows) to absorb the device_time_offset_secs measurement slop
    described in the module docstring. Each window is generated in the
    tag's own device-time space (`real_ts - offset`), not raw real time."""
    window_count = -(-int(hours * 3600) // ROTATION_PERIOD)  # ceil division
    offsets = [device_time_offset_secs + d * ROTATION_PERIOD
               for d in range(-offset_tolerance_windows, offset_tolerance_windows + 1)]
    out = []
    ts = start_ts
    for _ in range(window_count):
        for offset in offsets:
            out.append(generate_eid(eik, ts - offset).hex())
        ts += ROTATION_PERIOD
    return out


def load_known_tags(path: Path) -> list[dict]:
    """Each entry: {"name": str, "eik_hex": 64-char hex string (32 bytes),
    "device_time_offset_secs": int (optional, default 0 - see module
    docstring)}. See known_tags.example.json for the template - real keys
    go in known_tags.json, gitignored, same split as the ESP32 side's
    secrets.h/no_secrets.h."""
    with open(path, "r", encoding="utf-8") as f:
        tags = json.load(f)
    for tag in tags:
        eik = bytes.fromhex(tag["eik_hex"])
        if len(eik) != 32:
            raise ValueError(f"Tag \"{tag.get('name', '?')}\" has a {len(eik)}-byte EIK, expected 32.")
    return tags


class KnownTagCache:
    """Per-tag EID cache so per-sighting matching is a plain set lookup, not
    fresh crypto on every single BLE sighting.

    History: brain.py's _match_known_tag() first called build_candidate_table()
    (a full 24h forward sweep) per sighting - pegged a real Pi's CPU at 100%
    for 3+ hours and made it fall permanently behind real time. That was
    replaced with a per-sighting match_current_window() crypto check
    (~9.5ms/tag) - correct, but still redone from scratch on every single
    sighting from every ambient device, not just once per rotation. This
    class caches the actual EID *values*, not just makes the recompute
    cheaper:

    - `_current_eids`: the tag's current-window candidates (+-tolerance),
      refreshed only when real time crosses into a new ROTATION_PERIOD
      window (~every 17 minutes) - not per sighting. Tolerance absorbs the
      device_time_offset_secs measurement slop (see module docstring), and
      naturally already covers what would be a separate "next window"
      cache: +-1 is already inside the +-tolerance band.
    - `_baseline_eids`: the tag's factory reset-baseline window (+-tolerance),
      computed ONCE at construction and never refreshed - a battery pull
      always reverts the tag to this exact same device-time (see
      ../ESP32/FINDINGS.md section 5.2), so caching it lets a freshly-reset
      tag be recognized instantly without waiting to rediscover its offset
      from scratch. Only populated if reset_baseline_device_time is known
      (see FINDINGS.md section 5.1's table for this project's 3 real tags).
    """

    def __init__(self, name: str, eik_hex: str, device_time_offset_secs: int = 0,
                 reset_baseline_device_time: int | None = None,
                 offset_tolerance_windows: int = OFFSET_TOLERANCE_WINDOWS):
        self.name = name
        self.eik = bytes.fromhex(eik_hex)
        self.offset = device_time_offset_secs
        self.tolerance = offset_tolerance_windows
        self._cached_device_window: int | None = None
        self._current_eids: set[str] = set()
        self._baseline_eids: set[str] = set()
        if reset_baseline_device_time is not None:
            base = mask_timestamp(reset_baseline_device_time)
            self._baseline_eids = {
                generate_eid(self.eik, base + d * ROTATION_PERIOD).hex()
                for d in range(-offset_tolerance_windows, offset_tolerance_windows + 1)
            }

    def refresh(self, now: int) -> None:
        """No-op unless real time has actually crossed into a new rotation
        window since the last refresh - the cheap check that keeps this
        off the hot path."""
        device_window = mask_timestamp(now - self.offset)
        if device_window == self._cached_device_window:
            return
        self._cached_device_window = device_window
        self._current_eids = {
            generate_eid(self.eik, device_window + d * ROTATION_PERIOD).hex()
            for d in range(-self.tolerance, self.tolerance + 1)
        }

    def matches(self, eid_hex: str, now: int) -> bool:
        self.refresh(now)
        return eid_hex in self._current_eids or eid_hex in self._baseline_eids


def build_known_tag_caches(known_tags_path: Path,
                            offset_tolerance_windows: int = OFFSET_TOLERANCE_WINDOWS) -> list[KnownTagCache]:
    """Builds one KnownTagCache per entry in known_tags.json - call once at
    startup (see brain.py's Brain.__init__), not per sighting. If
    known_tags.json changes while the server is running, it needs a restart
    to pick up the change - same as any other code/config change in this
    project so far (see FINDINGS.md/README.md's deploy notes)."""
    return [
        KnownTagCache(
            name=tag["name"],
            eik_hex=tag["eik_hex"],
            device_time_offset_secs=tag.get("device_time_offset_secs", 0),
            reset_baseline_device_time=tag.get("reset_baseline_device_time"),
            offset_tolerance_windows=offset_tolerance_windows,
        )
        for tag in load_known_tags(known_tags_path)
    ]


def build_candidate_table(known_tags_path: Path, hours: int = DEFAULT_WINDOW_HOURS) -> dict:
    """The payload served to the hub - see server.py's /candidates route."""
    now = int(time.time())
    tags = load_known_tags(known_tags_path)
    return {
        "generated_at": now,
        "rotation_period": ROTATION_PERIOD,
        "window_hours": hours,
        "tags": [
            {
                "name": tag["name"],
                "candidates": compute_candidates(
                    bytes.fromhex(tag["eik_hex"]), now, hours,
                    device_time_offset_secs=tag.get("device_time_offset_secs", 0),
                ),
            }
            for tag in tags
        ],
    }
