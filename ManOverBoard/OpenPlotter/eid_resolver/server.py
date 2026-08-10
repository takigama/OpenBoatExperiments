"""
HTTP API + web UI for the MOB controller - this is what makes OpenPlotter
able to act as the "controller" in the hub-and-spoke design (see the
ManOverBoard project's design discussion): any scanner (the CYD's own
companion, a headless C3, eventually a phone) POSTs raw sightings here,
and this owns the actual missing-tag/alarm decision (brain.py, a Python
port of the CYD firmware's state machine) - the CYD keeps running its own
copy of that same logic locally as an always-available fallback; this is
the version that runs once more than one scanner is in the picture.

The Pi's own onboard Bluetooth also scans directly (ble_scanner.py, using
bleak/BlueZ) and feeds the same brain.record_sighting() pipeline as any
other scanner, tagged with source="pi-onboard" - so this is a real
scanning node in its own right, not just a passive sink; it works fully
standalone with no CYD or other hardware at all.

Deliberately stdlib-only (http.server), same reasoning as before - modest
traffic, not a service under real load, and the web UI is plain HTML forms
with a meta-refresh, matching the CYD's own debug web pages rather than
pulling in a UI framework.
"""

import html
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs

from eid_resolver import ble_scanner
from eid_resolver.brain import Brain
from eid_resolver.candidates import build_candidate_table

DEFAULT_PORT = 8734
KNOWN_TAGS_PATH = Path(__file__).parent / "known_tags.json"
MISSING_CHECK_INTERVAL_SECS = 1  # same cadence as the ESP32's loop()-driven check

brain = Brain(KNOWN_TAGS_PATH)


def _missing_check_loop():
    while True:
        for tag in brain.check_missing():
            label = tag.name if tag.is_known else tag.key
            sys.stderr.write(f"[MOB] Triggered for {label}\n")
        time.sleep(MISSING_CHECK_INTERVAL_SECS)


class ControllerHandler(BaseHTTPRequestHandler):
    def _send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, status: int, body_str: str) -> None:
        body = body_str.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self) -> dict:
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b""
        content_type = self.headers.get("Content-Type", "")
        if "application/json" in content_type:
            return json.loads(raw) if raw else {}
        # Plain HTML forms (the web UI) post as x-www-form-urlencoded.
        parsed = parse_qs(raw.decode("utf-8"))
        return {k: v[0] for k, v in parsed.items()}

    # ---- GET routes ----
    def do_GET(self):
        if self.path == "/":
            self._send_html(200, render_dashboard())
        elif self.path == "/debug":
            self._send_html(200, render_debug())
        elif self.path == "/settings":
            self._send_html(200, render_settings())
        elif self.path == "/api/tags":
            self._send_json(200, brain.to_json())
        elif self.path == "/candidates":
            try:
                self._send_json(200, build_candidate_table(KNOWN_TAGS_PATH))
            except FileNotFoundError:
                self._send_json(404, {"error": "known_tags.json not found - see README.md"})
            except (ValueError, KeyError) as e:
                self._send_json(500, {"error": f"known_tags.json is malformed: {e}"})
        else:
            self._send_json(404, {"error": "not found"})

    # ---- POST routes ----
    def do_POST(self):
        if self.path == "/sightings":
            data = self._read_body()
            try:
                eid = data["eid"]
                rssi = int(data.get("rssi", -100))
                source = data.get("source", "")
                if not eid or not isinstance(eid, str):
                    raise ValueError("empty eid")
                bytes.fromhex(eid)  # validates it's actually hex, not just non-empty
            except (KeyError, ValueError):
                self._send_json(400, {"error": "expected {\"eid\": non-empty hex_string, \"rssi\": int, \"source\": str}"})
                return
            tag = brain.record_sighting(eid, rssi, source)
            self._send_json(200, {"key": tag.key, "is_known": tag.is_known, "enrolled": tag.enrolled})
        elif self.path == "/settings/save":
            data = self._read_body()
            try:
                if "missing_secs" in data:
                    brain.set_missing_threshold(int(data["missing_secs"]))
                if "action_secs" in data:
                    brain.set_action_timeout(int(data["action_secs"]))
            except ValueError:
                self._send_json(400, {"error": "missing_secs/action_secs must be integers"})
                return
            self.send_response(303)
            self.send_header("Location", "/settings")
            self.end_headers()
        elif self.path in ("/tags/enroll", "/tags/unenroll", "/tags/silence", "/tags/reset_stats"):
            data = self._read_body()
            key = data.get("key", "")
            ok = {
                "/tags/enroll": brain.enroll,
                "/tags/unenroll": brain.unenroll,
                "/tags/silence": brain.silence,
                "/tags/reset_stats": brain.reset_stats,
            }[self.path](key)
            redirect_to = "/debug" if self.path == "/tags/reset_stats" else "/"
            if self.headers.get("Content-Type", "").startswith("application/x-www-form-urlencoded") or not self.headers.get("Content-Type"):
                self.send_response(303)
                self.send_header("Location", redirect_to)
                self.end_headers()
            else:
                self._send_json(200 if ok else 404, {"ok": ok})
        elif self.path == "/stats/reset_all":
            brain.reset_all_stats()
            self.send_response(303)
            self.send_header("Location", "/debug")
            self.end_headers()
        else:
            self._send_json(404, {"error": "not found"})

    def log_message(self, format, *args):  # noqa: A002 - matches BaseHTTPRequestHandler's signature
        sys.stderr.write("[controller] %s - %s\n" % (self.address_string(), format % args))


def render_dashboard() -> str:
    state = brain.to_json()
    rows = ""
    for tag in state["tags"]:
        label = html.escape(tag["name"] or tag["key"][:6] + "..." + tag["key"][-6:])
        status = "known" if tag["is_known"] else ("enrolled" if tag["enrolled"] else "ignored")
        if tag["alarming"]:
            status = "ESCALATED" if tag["escalated"] else "ALARM"
        last_seen = f"{tag['last_seen_secs_ago']}s ago" if tag["last_seen_secs_ago"] is not None else "never"
        toggle_action = "/tags/unenroll" if (tag["enrolled"] and not tag["is_known"]) else "/tags/enroll"
        toggle_label = "Un-enroll" if (tag["enrolled"] and not tag["is_known"]) else "Enroll"
        toggle_form = "" if tag["is_known"] else (
            f"<form method='POST' action='{toggle_action}' style='display:inline'>"
            f"<input type='hidden' name='key' value='{html.escape(tag['key'])}'>"
            f"<input type='submit' value='{toggle_label}'></form>"
        )
        silence_form = (
            f"<form method='POST' action='/tags/silence' style='display:inline'>"
            f"<input type='hidden' name='key' value='{html.escape(tag['key'])}'>"
            f"<input type='submit' value='Silence'></form>"
        )
        rows += (
            f"<tr><td>{label}</td><td>{status}</td><td>{tag['rssi']}</td>"
            f"<td>{last_seen}</td><td>{html.escape(tag['source'])}</td>"
            f"<td>{toggle_form}</td><td>{silence_form}</td></tr>"
        )
    return f"""<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta http-equiv='refresh' content='3'>
<title>MOB Controller</title></head><body>
<h2>MOB Controller</h2>
<p>Missing-tag threshold: {state['missing_threshold_secs']}s</p>
<table border=1 cellpadding=6>
<tr><th>Tag</th><th>Status</th><th>RSSI</th><th>Last seen</th><th>Source</th><th></th><th></th></tr>
{rows}
</table>
<p><a href='/api/tags'>Raw JSON</a> | <a href='/candidates'>Candidate table</a> | <a href='/debug'>Debug stats</a> | <a href='/settings'>Settings</a></p>
</body></html>"""


def render_settings() -> str:
    state = brain.to_json()
    return f"""<!DOCTYPE html><html><head><meta charset='utf-8'>
<title>MOB Settings</title></head><body>
<h2>MOB Settings</h2>
<form method='POST' action='/settings/save'>
Missing-tag threshold (seconds before a silent tag is declared MOB):
<input name='missing_secs' type='number' min='2' max='3600' value='{state["missing_threshold_secs"]}'><br><br>
Detected -&gt; Escalated timeout (seconds):
<input name='action_secs' type='number' min='1' max='3600' value='{state["action_timeout_secs"]}'><br><br>
<input type='submit' value='Save'></form>
<p><a href='/'>Back</a></p>
</body></html>"""


def render_debug() -> str:
    state = brain.to_json()
    rows = ""
    for tag in state["tags"]:
        label = html.escape(tag["name"] or tag["key"][:6] + "..." + tag["key"][-6:])
        current_gap = tag["last_seen_secs_ago"] if tag["last_seen_secs_ago"] is not None else "-"
        first_seen = f"{tag['first_seen_secs_ago']}s ago" if tag["first_seen_secs_ago"] is not None else "-"
        min_gap = tag["min_gap_secs"] if tag["min_gap_secs"] is not None else "-"
        avg_gap = tag["avg_gap_secs"] if tag["avg_gap_secs"] is not None else "-"
        reset_form = (
            f"<form method='POST' action='/tags/reset_stats' style='display:inline'>"
            f"<input type='hidden' name='key' value='{html.escape(tag['key'])}'>"
            f"<input type='submit' value='Reset'></form>"
        )
        rows += (
            f"<tr><td>{label}</td><td>{tag['sighting_count']}</td>"
            f"<td>{min_gap}</td><td>{avg_gap}</td><td>{tag['max_gap_secs']}</td>"
            f"<td>{current_gap}</td>"
            f"<td>{first_seen}</td><td>{tag['rssi']}</td>"
            f"<td>{html.escape(tag['source'])}</td><td>{reset_form}</td></tr>"
        )
    # Per-source breakdown - the combined table above mixes every scanner's
    # sightings into one gap sequence per tag, which hides exactly the
    # comparison this is for (does adding a scanner - a CYD over serial, a
    # boat's worth of ESP32s - actually improve reception, or is one
    # source's gap being masked by another's better timing).
    source_rows = ""
    for tag in state["tags"]:
        label = html.escape(tag["name"] or tag["key"][:6] + "..." + tag["key"][-6:])
        for src_name, src in sorted(tag["per_source"].items()):
            src_current = src["last_seen_secs_ago"] if src["last_seen_secs_ago"] is not None else "-"
            src_min = src["min_gap_secs"] if src["min_gap_secs"] is not None else "-"
            src_avg = src["avg_gap_secs"] if src["avg_gap_secs"] is not None else "-"
            source_rows += (
                f"<tr><td>{label}</td><td>{html.escape(src_name)}</td>"
                f"<td>{src['sighting_count']}</td>"
                f"<td>{src_min}</td><td>{src_avg}</td><td>{src['max_gap_secs']}</td>"
                f"<td>{src_current}</td></tr>"
            )
    return f"""<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta http-equiv='refresh' content='3'>
<title>MOB Debug Stats</title></head><body>
<h2>MOB Debug Stats</h2>
<p>Missing-tag threshold: {state['missing_threshold_secs']}s</p>
<p>Min/avg/max gap = shortest/typical/longest time between two consecutive
sightings of that tag - a rough feel for how good reception has been.
A tag advertising reliably should show a small, consistent min/avg gap;
frequent big gaps (getting close to the missing-tag threshold above) mean
beacons are being missed.</p>
<p><form method='POST' action='/stats/reset_all' style='display:inline'>
<input type='submit' value='Reset All'></form>
Resetting clears sighting counts and min/avg/max gap for a clean
measurement window (e.g. after moving an antenna) - it does not affect
enrollment or alarm state.</p>
<table border=1 cellpadding=6>
<tr><th>Tag</th><th>Sightings</th><th>Min gap (s)</th><th>Avg gap (s)</th>
<th>Max gap (s)</th><th>Current gap (s)</th>
<th>First seen</th><th>Last RSSI</th><th>Last source</th><th></th></tr>
{rows}
</table>
<h3>Per-source breakdown</h3>
<p>Same gap stats, but split out per scanner instead of mixed together -
this is what actually shows whether one scanner is doing better or worse
than another for the same physical tag.</p>
<table border=1 cellpadding=6>
<tr><th>Tag</th><th>Source</th><th>Sightings</th><th>Min gap (s)</th>
<th>Avg gap (s)</th><th>Max gap (s)</th><th>Current gap (s)</th></tr>
{source_rows}
</table>
<p><a href='/'>Dashboard</a> | <a href='/api/tags'>Raw JSON</a></p>
</body></html>"""


def run(port: int = DEFAULT_PORT):
    threading.Thread(target=_missing_check_loop, daemon=True).start()
    ble_scanner.start(brain)
    server = ThreadingHTTPServer(("0.0.0.0", port), ControllerHandler)
    print(f"MOB controller listening on http://0.0.0.0:{port}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.shutdown()


if __name__ == "__main__":
    run()
