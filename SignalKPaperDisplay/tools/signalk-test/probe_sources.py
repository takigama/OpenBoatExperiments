#!/usr/bin/env python3
"""Two sources publish the same path on a real SignalK server (my own test path, no priority
configured). What can a client see? REST per-source values; websocket default; sourcePolicy 'all'."""
import os
import asyncio, json, sys, time, urllib.request, urllib.error
import websockets

HOST = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("SK_HOST", "127.0.0.1:3001")
PATH = "electrical.switches.bank.100.2.state"
SRC = ["test-gps-a", "test-gps-b"]

def rest(path):
    url = f"http://{HOST}/signalk/v1/api/vessels/self/" + path.replace(".", "/")
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()[:100]

def delta(src, v):
    return json.dumps({"context": "vessels.self", "updates": [{"$source": src, "values": [{"path": PATH, "value": v}]}]})

async def publisher(stop):
    async with websockets.connect(f"ws://{HOST}/signalk/v1/stream?subscribe=none", ping_interval=None) as ws:
        await ws.recv()
        n = 0
        while not stop.is_set():
            n += 1
            await ws.send(delta(SRC[n % 2], n % 2 == 0))   # alternate sources, alternate values
            await asyncio.sleep(0.4)

async def listen(label, url, sub, secs=4):
    seen = {}
    async with websockets.connect(url, ping_interval=None) as ws:
        await ws.recv()
        await ws.send(json.dumps({"context": "vessels.self", "subscribe": [sub]}))
        end = time.time() + secs
        while time.time() < end:
            try:
                m = json.loads(await asyncio.wait_for(ws.recv(), end - time.time()))
            except asyncio.TimeoutError:
                break
            for u in m.get("updates", []):
                src = u.get("$source") or (u.get("source") or {}).get("label")
                for v in u.get("values", []):
                    if v["path"] == PATH:
                        seen[src] = seen.get(src, 0) + 1
    print(f"  {label}: {seen if seen else 'nothing'}")

async def main():
    print("server", HOST)
    stop = asyncio.Event()
    pub = asyncio.create_task(publisher(stop))
    await asyncio.sleep(2)
    base = f"ws://{HOST}/signalk/v1/stream?subscribe=none"
    print("\n-- websocket, what a client sees while two sources alternate")
    await listen("default subscription", base, {"path": PATH, "policy": "instant"})
    await listen("sourcePolicy:'all' in the subscription", base, {"path": PATH, "policy": "instant", "sourcePolicy": "all"})
    await listen("sourcePolicy=all on the connection", base + "&sourcePolicy=all", {"path": PATH, "policy": "instant"})
    stop.set()
    await pub

    print("\n-- REST")
    code, body = rest(PATH + ".values")
    print("  /values ->", code, json.dumps(body)[:300])
    code, body = rest(PATH)
    print("  the path ->", code, json.dumps(body)[:300])

asyncio.run(main())
