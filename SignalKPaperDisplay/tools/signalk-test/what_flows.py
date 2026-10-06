import os
import asyncio, json, time, urllib.request
import websockets

H = os.environ.get("SK_HOST", "127.0.0.1:3001")

async def main():
    paths, srcs, vessels, n_msgs, n_vals = {}, {}, {}, 0, 0
    async with websockets.connect(f"ws://{H}/signalk/v1/stream?subscribe=all", ping_interval=None) as ws:
        hello = json.loads(await ws.recv())
        self_id = hello.get("self")
        t0 = time.time()
        while time.time() - t0 < 12:
            try:
                m = json.loads(await asyncio.wait_for(ws.recv(), 12 - (time.time() - t0)))
            except asyncio.TimeoutError:
                break
            n_msgs += 1
            ctx = m.get("context", "")
            for u in m.get("updates", []):
                s = u.get("$source") or (u.get("source") or {}).get("label")
                for v in u.get("values", []):
                    n_vals += 1
                    key = ("own" if ctx in ("vessels.self", self_id) else "other") + ":" + v["path"]
                    paths[key] = paths.get(key, 0) + 1
                    if s:
                        srcs[s] = srcs.get(s, 0) + 1
                    if ctx not in ("vessels.self", self_id):
                        vessels[ctx] = vessels.get(ctx, 0) + 1
    secs = 12
    own = sorted(k[4:] for k in paths if k.startswith("own:"))
    print(f"server {H}: {n_msgs} messages / {n_vals} values in {secs} s  (about {n_vals/secs:.0f} values per second)")
    print(f"own vessel: {len(own)} different paths")
    groups = {}
    for p in own:
        g = ".".join(p.split(".")[:2]) if p.startswith(("electrical", "environment", "navigation", "propulsion", "tanks", "steering")) else p.split(".")[0]
        groups.setdefault(g, 0)
        groups[g] += 1
    for g, c in sorted(groups.items()):
        print(f"   {g:<34} {c}")
    print(f"other vessels (AIS targets) seen: {len(vessels)}")
    for v, c in sorted(vessels.items()):
        print(f"   {v}  ({c} values)")
    print("sources:", dict(sorted(srcs.items(), key=lambda x: -x[1])))
    # how many were updated in this window vs. slower ones
    slow = [p for p in own if paths["own:" + p] <= 2]
    print(f"paths that updated only once or twice in {secs} s (slow ones): {len(slow)}")

asyncio.run(main())
