#!/usr/bin/env python3
"""Failover timing on the throwaway SignalK server, measured as a client.
For each timeout: both GPS sources publish at 1 Hz; a client subscribed to everything must see only the
higher-ranked one (A). Then A is stopped and we time how long until the client first sees B; then A is
restarted and we time how long until the client sees A again."""
import os
import asyncio, json, math, statistics, sys, time, urllib.request
import websockets

H = os.environ.get("SK_HOST", "127.0.0.1:3001")
TIMEOUTS_MS = [2000, 5000, 10000]
REPS = 3
KN = 0.514444
SRC = {
    "A": {"label": "test-gps-A", "lat": -33.8450, "lon": 151.2400, "cog": math.pi / 2, "sog": 5 * KN},
    "B": {"label": "test-gps-B", "lat": -33.8450 - 5.0 / 60, "lon": 151.2400, "cog": 3 * math.pi / 2, "sog": 8 * KN},
}
PATHS = ["navigation.position", "navigation.courseOverGroundTrue", "navigation.speedOverGround"]

def put_priority(timeout_ms):
    body = {"groups": [], "defaults": {}, "overrides": {p: [{"sourceRef": "test-gps-A", "timeout": timeout_ms}, {"sourceRef": "test-gps-B", "timeout": timeout_ms}] for p in PATHS}}
    r = urllib.request.Request(f"http://{H}/skServer/priorities", method="PUT", data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    urllib.request.urlopen(r, timeout=8).read()

def msg(key):
    s = SRC[key]
    vals = [{"path": "navigation.position", "value": {"latitude": s["lat"], "longitude": s["lon"]}},
            {"path": "navigation.courseOverGroundTrue", "value": s["cog"]},
            {"path": "navigation.speedOverGround", "value": s["sog"]}]
    return json.dumps({"context": "vessels.self", "updates": [{"$source": s["label"], "values": vals}]})

async def publisher(key, offset):
    async with websockets.connect(f"ws://{H}/signalk/v1/stream?subscribe=none", ping_interval=None) as ws:
        await ws.recv()
        await asyncio.sleep(offset)
        while True:
            await ws.send(msg(key))
            await asyncio.sleep(1.0)

seen = []   # (monotonic time, source letter) for each navigation.position update the client receives

async def client():
    async with websockets.connect(f"ws://{H}/signalk/v1/stream?subscribe=all", ping_interval=None) as ws:
        async for raw in ws:
            m = json.loads(raw)
            now = time.monotonic()
            for u in m.get("updates", []):
                src = (u.get("$source") or "")[-1:]
                for v in u.get("values", []):
                    if v["path"] == "navigation.position":
                        seen.append((now, src))

def sources_between(t0, t1):
    return sorted({s for (t, s) in seen if t0 <= t <= t1})

async def first_after(letter, t0, limit):
    end = time.monotonic() + limit
    while time.monotonic() < end:
        for (t, s) in seen:
            if t > t0 and s == letter:
                return t
        await asyncio.sleep(0.02)
    return None

def last_seen(letter, before):
    ts = [t for (t, s) in seen if s == letter and t <= before]
    return max(ts) if ts else None

async def main():
    results = {}
    cl = asyncio.create_task(client())
    await asyncio.sleep(1)
    for T in TIMEOUTS_MS:
        put_priority(T)
        await asyncio.sleep(1)
        print(f"\n=== fallback timeout {T/1000:g} s (ranking: A first, B second)", flush=True)
        results[T] = {"failover": [], "back": []}
        for rep in range(1, REPS + 1):
            seen.clear()
            tA = asyncio.create_task(publisher("A", 0.0))
            tB = asyncio.create_task(publisher("B", 0.5))
            await asyncio.sleep(5)
            t_now = time.monotonic()
            both = sources_between(t_now - 4, t_now)
            ok = both == ["A"]
            # stop A
            t_stop = time.monotonic()
            tA.cancel()
            await asyncio.sleep(0)
            lastA = last_seen("A", t_stop)
            tb = await first_after("B", t_stop, T / 1000 + 8)
            if tb is None:
                print(f"  rep {rep}: client saw {both} with both running; B NEVER appeared after A stopped", flush=True)
                tB.cancel(); await asyncio.sleep(1); continue
            failover = tb - lastA
            await asyncio.sleep(3)
            # restart A
            t_back = time.monotonic()
            tA = asyncio.create_task(publisher("A", 0.0))
            ta = await first_after("A", t_back, 8)
            back = (ta - t_back) if ta else float("nan")
            await asyncio.sleep(3)
            t_end = time.monotonic()
            after = sources_between(t_back + 1.5, t_end)
            print(f"  rep {rep}: both running -> client saw {both} ({'only the higher-ranked, as expected' if ok else 'UNEXPECTED'});  "
                  f"A stopped -> B seen {failover:.2f} s after A's last update ({tb - t_stop:.2f} s after the stop);  "
                  f"A restarted -> A seen {back:.2f} s later, then client saw {after}", flush=True)
            results[T]["failover"].append(failover)
            results[T]["back"].append(back)
            tA.cancel(); tB.cancel()
            await asyncio.sleep(3)
    cl.cancel()
    print("\n=== summary (time from A's last update, as seen by a client, to the first update from B)")
    for T, r in results.items():
        f = r["failover"]
        if f:
            print(f"  timeout {T/1000:>4g} s: failover min {min(f):.2f} / median {statistics.median(f):.2f} / max {max(f):.2f} s;  "
                  f"switch back to A after restart: {', '.join(f'{x:.2f}' for x in r['back'])} s")
    put_priority(5000)
    print("\nranking left at A first, B second, timeout 5 s; no publishers are running now.")

asyncio.run(main())
