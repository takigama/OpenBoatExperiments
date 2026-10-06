#!/usr/bin/env python3
"""Set (or clear) a source ranking on the throwaway test server, then show what a client now gets.
usage: set_priority.py [first_source] [second_source]    (default A then B);  set_priority.py clear"""
import os
import asyncio, json, sys, time, urllib.request
import websockets

H = os.environ.get("SK_HOST", "127.0.0.1:3001")
PATHS = ["navigation.position", "navigation.courseOverGroundTrue", "navigation.speedOverGround"]

def req(method, path, body=None):
    r = urllib.request.Request(f"http://{H}{path}", method=method, data=json.dumps(body).encode() if body is not None else None,
                               headers={"Content-Type": "application/json"} if body is not None else {})
    with urllib.request.urlopen(r, timeout=8) as x:
        t = x.read().decode()
        return x.status, (json.loads(t) if t.strip().startswith(("{", "[")) else t)

async def listen(secs=5):
    seen = {}
    async with websockets.connect(f"ws://{H}/signalk/v1/stream?subscribe=all", ping_interval=None) as ws:
        end = time.time() + secs
        while time.time() < end:
            try:
                m = json.loads(await asyncio.wait_for(ws.recv(), end - time.time()))
            except asyncio.TimeoutError:
                break
            for u in m.get("updates", []):
                src = u.get("$source")
                for v in u.get("values", []):
                    if v["path"] == "navigation.position":
                        seen[src] = seen.get(src, 0) + 1
    return seen

args = sys.argv[1:]
if args[:1] == ["clear"]:
    body = {"groups": [], "overrides": {}, "defaults": {}}
    print("clearing the ranking")
else:
    first = f"test-gps-{args[0] if args else 'A'}"
    second = f"test-gps-{args[1] if len(args) > 1 else 'B'}"
    body = {"groups": [], "defaults": {}, "overrides": {p: [{"sourceRef": first, "timeout": 5000}, {"sourceRef": second, "timeout": 5000}] for p in PATHS}}
    print(f"ranking {first} first, then {second} (takes over after 5 s of silence), for {', '.join(p.split('.')[-1] for p in PATHS)}")
code, out = req("PUT", "/skServer/priorities", body)
print("PUT ->", code, out if isinstance(out, str) else "ok")
print("now stored:", json.dumps(req("GET", "/skServer/priorities")[1])[:300])
time.sleep(2)
print("a client subscribed to everything, over 5 s, receives navigation.position from:", asyncio.run(listen()))
print("REST headline position source:", req("GET", "/signalk/v1/api/vessels/self/navigation/position")[1].get("$source"))
