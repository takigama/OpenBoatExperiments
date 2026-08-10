"""
Python port of the CYD firmware's MOB decision logic (see
../../ESP32/src/display_mob_test.cpp) - this is what makes OpenPlotter able
to act as the "controller" in the hub-and-spoke design: any scanner
(CYD's own companion, a headless C3, eventually a phone) just POSTs raw
sightings here; this module owns the actual missing-tag/alarm decision,
same as the ESP32 does locally. The CYD keeps its own local copy of this
same logic unchanged as an always-available fallback (see the
"graceful degradation" design discussion) - this isn't a replacement for
that, it's the version that runs when more than one scanner/board is
in the picture.

Two tag identity modes, exactly as discussed:
- KNOWN (crypto-resolved): identified by a stable name from known_tags.json.
  A sighting matches by checking the EID against that tag's *already
  precomputed* rolling candidate window (see candidates.py) - this is why
  known tags never need the swap-heuristic at all: a rotation's new EID is
  already sitting in the candidate list before it's even broadcast, so
  there's no timing race to get wrong the way there was on the ESP32
  before it moved to deferred, missing-time-only guessing.
- UNKNOWN (heuristic): identified by its current raw EID, exactly like the
  ESP32's SeenTag - enrolled/ignored, first/last-seen, and the same
  find_ignore_swap_candidate() logic ported directly.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from threading import Lock

from eid_resolver.candidates import build_known_tag_caches

STATE_PATH = Path(__file__).parent / "brain_state.json"

DEFAULT_MISSING_THRESHOLD_SECS = 6  # same default as the ESP32 firmware
DEFAULT_ACTION_TIMEOUT_SECS = 10  # same default as the ESP32's DETECTED -> ACTIONED escalation
SWAP_WINDOW_MULTIPLIER = 2  # same "2x missing threshold" rule as the ESP32


@dataclass
class SourceStats:
    """Per-scanner-source reception stats for a tag, tracked separately
    from TrackedTag's own combined (any-source) stats below. The combined
    stats mix every scanner's sightings into one gap sequence - fine for
    the missing-tag check (any scanner hearing the tag counts), but it
    hides exactly the comparison this exists for: is a given scanner (the
    Pi's onboard Bluetooth vs. a CYD wired in over serial vs., eventually,
    a boat's worth of ESP32s) actually doing better or worse on its own."""
    last_seen: float = 0.0
    sighting_count: int = 0
    max_gap_secs: float = 0.0
    min_gap_secs: float = 0.0
    gap_sum_secs: float = 0.0
    gap_count: int = 0


@dataclass
class TrackedTag:
    key: str  # stable name for known tags, current EID hex for unknown ones
    is_known: bool
    enrolled: bool
    name: str = ""  # only ever set for known tags
    rssi: int = -100
    first_seen: float = 0.0  # 0 = pre-existing (loaded from persisted state), matches ESP32's rule
    last_seen: float = 0.0
    # True once check_missing() has declared this tag MOB - stays true (like
    # the CYD's flashing alarm screen) until silence() acknowledges it,
    # rather than firing once and being forgotten. While alarming,
    # check_missing() skips re-evaluating it, same as the ESP32 only
    # running its missing-check from the idle NORMAL_LIST screen.
    alarming: bool = False
    alarm_started: float = 0.0  # 0.0 when not alarming - for the escalation timeout
    source: str = ""  # which scanner reported it last, for multi-scanner visibility
    sighting_count: int = 0
    max_gap_secs: float = 0.0  # longest time between two consecutive sightings, for /debug
    min_gap_secs: float = 0.0  # shortest gap seen - a rough feel for the tag's real advertising interval
    gap_sum_secs: float = 0.0  # sum of every recorded gap, for the average
    gap_count: int = 0  # how many gaps have been recorded (sightings - 1)
    per_source: dict[str, SourceStats] = field(default_factory=dict)


class Brain:
    def __init__(self, known_tags_path: Path, missing_threshold_secs: int = DEFAULT_MISSING_THRESHOLD_SECS,
                 action_timeout_secs: int = DEFAULT_ACTION_TIMEOUT_SECS):
        self.known_tags_path = known_tags_path
        self.missing_threshold_secs = missing_threshold_secs
        self.action_timeout_secs = action_timeout_secs
        self.tags: dict[str, TrackedTag] = {}
        self._lock = Lock()
        # Built once here, not per sighting - see candidates.py's
        # KnownTagCache/build_known_tag_caches docstrings. A known_tags.json
        # edit needs a server restart to take effect, same as any other
        # code/config change in this project.
        self._known_tag_caches = (
            build_known_tag_caches(known_tags_path) if known_tags_path.exists() else []
        )
        self._load_state()

    # ---- persistence - only unknown-tag enrollment and the two tunable
    # timeouts survive a restart, same split as the ESP32's NVS-backed
    # enrolled list vs. session-only ignored tags. Known tags don't need
    # anything persisted here at all - their trust comes from being in
    # known_tags.json, not from this file. ----
    def _load_state(self):
        if not STATE_PATH.exists():
            return
        with open(STATE_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        # last_seen starts at "now" (not 0.0) so a reloaded tag gets a full
        # missing_threshold_secs grace period before it can be declared
        # missing - unlike the ESP32's millis()-since-boot clock, time.time()
        # is epoch time, so leaving this at 0.0 would make it look billions
        # of seconds overdue on the very first check_missing() pass after a
        # restart. first_seen stays 0.0 deliberately (marks it pre-existing,
        # ineligible as a swap candidate - see _find_ignore_swap_candidate()).
        for key in data.get("enrolled_unknown_keys", []):
            self.tags[key] = TrackedTag(key=key, is_known=False, enrolled=True,
                                         first_seen=0.0, last_seen=time.time())
        self.missing_threshold_secs = data.get("missing_threshold_secs", self.missing_threshold_secs)
        self.action_timeout_secs = data.get("action_timeout_secs", self.action_timeout_secs)

    def _save_state(self):
        enrolled_unknown = [t.key for t in self.tags.values() if not t.is_known and t.enrolled]
        with open(STATE_PATH, "w", encoding="utf-8") as f:
            json.dump({
                "enrolled_unknown_keys": enrolled_unknown,
                "missing_threshold_secs": self.missing_threshold_secs,
                "action_timeout_secs": self.action_timeout_secs,
            }, f)

    def set_missing_threshold(self, secs: int) -> None:
        with self._lock:
            if secs > 0:
                self.missing_threshold_secs = secs
                self._save_state()

    def set_action_timeout(self, secs: int) -> None:
        with self._lock:
            if secs > 0:
                self.action_timeout_secs = secs
                self._save_state()

    # ---- known-tag candidate matching ----
    def _match_known_tag(self, eid_hex: str, now: float) -> str | None:
        """Returns the known tag's name if eid_hex matches its current
        window (or its factory reset-baseline window), else None. Checks
        against each KnownTagCache built once at startup - a plain set
        lookup on every sighting, with fresh crypto only run once per tag
        per ~17-minute rotation (see candidates.py's KnownTagCache), not on
        every single BLE sighting from every ambient device. Earlier
        versions of this method rebuilt a full 24h forward candidate table,
        then re-ran the crypto check itself, per sighting - both expensive
        enough to peg a real Pi's CPU at 100% and fall permanently behind
        real time."""
        now_int = int(now)
        for cache in self._known_tag_caches:
            if cache.matches(eid_hex, now_int):
                return cache.name
        return None

    # ---- recording a sighting - the single entry point every scanner's
    # reports flow through, mirroring process_pending_eid()/
    # find_or_create_tag_slot() on the ESP32 ----
    def _touch(self, tag: TrackedTag, now: float, rssi: int, source: str) -> None:
        if tag.last_seen > 0.0:
            gap = now - tag.last_seen
            if gap > tag.max_gap_secs:
                tag.max_gap_secs = gap
            if tag.gap_count == 0 or gap < tag.min_gap_secs:
                tag.min_gap_secs = gap
            tag.gap_sum_secs += gap
            tag.gap_count += 1
        tag.sighting_count += 1
        tag.rssi = rssi
        tag.last_seen = now
        tag.source = source
        # Mirrors mark_tag_seen() on the ESP32: a reappearing tag auto-clears
        # its own alarm - without this, any tag that ever crosses the missing
        # threshold once (e.g. from a normal BLE scan gap) stays stuck in
        # ALARM forever, since nothing else ever clears it.
        if tag.alarming:
            tag.alarming = False
            tag.alarm_started = 0.0

        # Per-source breakdown - see SourceStats' docstring for why this is
        # tracked independently of the combined stats above.
        src = tag.per_source.setdefault(source, SourceStats())
        if src.last_seen > 0.0:
            src_gap = now - src.last_seen
            if src_gap > src.max_gap_secs:
                src.max_gap_secs = src_gap
            if src.gap_count == 0 or src_gap < src.min_gap_secs:
                src.min_gap_secs = src_gap
            src.gap_sum_secs += src_gap
            src.gap_count += 1
        src.sighting_count += 1
        src.last_seen = now

    def record_sighting(self, eid_hex: str, rssi: int, source: str = "") -> TrackedTag:
        now = time.time()
        with self._lock:
            known_name = self._match_known_tag(eid_hex, now)
            if known_name is not None:
                tag = self.tags.get(known_name)
                if tag is None:
                    tag = TrackedTag(key=known_name, is_known=True, enrolled=True,
                                      name=known_name, first_seen=now)
                    self.tags[known_name] = tag
                self._touch(tag, now, rssi, source)
                return tag

            tag = self.tags.get(eid_hex)
            if tag is None:
                tag = TrackedTag(key=eid_hex, is_known=False, enrolled=False, first_seen=now)
                self.tags[eid_hex] = tag
            self._touch(tag, now, rssi, source)
            return tag

    # ---- the swap-heuristic, ported directly from find_ignore_swap_candidate() ----
    def _find_ignore_swap_candidate(self, now: float) -> TrackedTag | None:
        window_secs = SWAP_WINDOW_MULTIPLIER * self.missing_threshold_secs
        best: TrackedTag | None = None
        for tag in self.tags.values():
            if tag.is_known or tag.enrolled:
                continue
            if tag.first_seen == 0.0:
                continue  # pre-existing (loaded from state), not a fresh arrival
            visible_secs = now - tag.first_seen
            if visible_secs > window_secs:
                continue
            if best is None or tag.first_seen > best.first_seen:
                best = tag
        return best

    # ---- the missing-tag check, ported from check_for_missing_tags() -
    # returns any tags that newly started alarming this pass (already-
    # alarming tags are skipped, same as the ESP32 only running this check
    # from the idle NORMAL_LIST screen) ----
    def check_missing(self) -> list[TrackedTag]:
        now = time.time()
        newly_triggered = []
        with self._lock:
            for tag in self.tags.values():
                if not tag.enrolled or tag.alarming:
                    continue
                if now - tag.last_seen < self.missing_threshold_secs:
                    continue

                if not tag.is_known:
                    candidate = self._find_ignore_swap_candidate(now)
                    if candidate is not None:
                        # Just flip enrolled status on each - same as the
                        # ESP32's unenroll_tag(key) + enroll_tag(candidate_key,
                        # "") - neither tag's own key/identity changes at all.
                        tag.enrolled = False
                        candidate.enrolled = True
                        self._save_state()
                        continue

                tag.alarming = True
                tag.alarm_started = now
                newly_triggered.append(tag)
        return newly_triggered

    def silence(self, key: str) -> bool:
        """Mirrors silence_alarm() for unknown tags - un-enrolls outright,
        same "dealt with -> back to ignored" semantics as the ESP32. Known
        tags can't be un-enrolled the same way (their trust comes from
        known_tags.json, not a toggle), so silencing one instead resets its
        last-seen clock, giving it a full fresh missing-threshold period
        before it can alarm again rather than either nagging again next
        pass or never being checked again."""
        with self._lock:
            tag = self.tags.get(key)
            if tag is None:
                return False
            tag.alarming = False
            tag.alarm_started = 0.0
            if tag.is_known:
                tag.last_seen = time.time()
            else:
                tag.enrolled = False
                self._save_state()
            return True

    def reset_stats(self, key: str) -> bool:
        """Zeroes a tag's reception stats (sighting count, min/avg/max gap)
        for the /debug page - useful after moving an antenna or wanting a
        clean measurement window, without losing enrollment/alarm state.
        first_seen is reset to now so first_seen_secs_ago reads as "time
        since this reset"; last_seen is left untouched since it reflects a
        real event, not a stat."""
        with self._lock:
            tag = self.tags.get(key)
            if tag is None:
                return False
            tag.sighting_count = 0
            tag.max_gap_secs = 0.0
            tag.min_gap_secs = 0.0
            tag.gap_sum_secs = 0.0
            tag.gap_count = 0
            tag.first_seen = time.time()
            tag.per_source = {}
            return True

    def reset_all_stats(self) -> None:
        with self._lock:
            now = time.time()
            for tag in self.tags.values():
                tag.sighting_count = 0
                tag.max_gap_secs = 0.0
                tag.min_gap_secs = 0.0
                tag.gap_sum_secs = 0.0
                tag.gap_count = 0
                tag.first_seen = now
                tag.per_source = {}

    def enroll(self, key: str) -> bool:
        with self._lock:
            tag = self.tags.get(key)
            if tag is None or tag.is_known:
                return False
            tag.enrolled = True
            self._save_state()
            return True

    def unenroll(self, key: str) -> bool:
        with self._lock:
            tag = self.tags.get(key)
            if tag is None or tag.is_known:
                return False
            tag.enrolled = False
            self._save_state()
            return True

    def to_json(self) -> dict:
        with self._lock:
            now = time.time()
            return {
                "generated_at": now,
                "missing_threshold_secs": self.missing_threshold_secs,
                "action_timeout_secs": self.action_timeout_secs,
                "tags": [
                    {
                        "key": t.key,
                        "name": t.name,
                        "is_known": t.is_known,
                        "enrolled": t.enrolled,
                        "alarming": t.alarming,
                        "escalated": t.alarming and t.alarm_started > 0.0
                                     and (now - t.alarm_started) >= self.action_timeout_secs,
                        "rssi": t.rssi,
                        "last_seen_secs_ago": round(now - t.last_seen, 1) if t.last_seen else None,
                        "source": t.source,
                        "sighting_count": t.sighting_count,
                        "max_gap_secs": round(t.max_gap_secs, 1),
                        "min_gap_secs": round(t.min_gap_secs, 1) if t.gap_count else None,
                        "avg_gap_secs": round(t.gap_sum_secs / t.gap_count, 1) if t.gap_count else None,
                        "first_seen_secs_ago": round(now - t.first_seen, 1) if t.first_seen else None,
                        "per_source": {
                            src_name: {
                                "sighting_count": s.sighting_count,
                                "max_gap_secs": round(s.max_gap_secs, 1),
                                "min_gap_secs": round(s.min_gap_secs, 1) if s.gap_count else None,
                                "avg_gap_secs": round(s.gap_sum_secs / s.gap_count, 1) if s.gap_count else None,
                                "last_seen_secs_ago": round(now - s.last_seen, 1) if s.last_seen else None,
                            }
                            for src_name, s in t.per_source.items()
                        },
                    }
                    for t in self.tags.values()
                ],
            }
