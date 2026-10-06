import asyncio, json, os, sys, time
import websockets

async def show(host, label):
    print(f"=== {label}  ({host})")
    async with websockets.connect(f"ws://{host}/signalk/v1/stream?subscribe=all", ping_interval=None) as ws:
        hello = json.loads(await ws.recv())
        print("hello:", json.dumps(hello)[:300])
        ctxs, with_ts, without_ts, shown = {}, 0, 0, 0
        end = time.time() + 6
        while time.time() < end:
            try:
                m = json.loads(await asyncio.wait_for(ws.recv(), end - time.time()))
            except asyncio.TimeoutError:
                break
            if "updates" not in m:
                continue
            ctxs[m.get("context")] = ctxs.get(m.get("context"), 0) + 1
            for u in m["updates"]:
                if "timestamp" in u: with_ts += 1
                else: without_ts += 1
            if shown < 3 and any(v["path"] == "navigation.position" for u in m["updates"] for v in u.get("values", [])):
                print("a position message:", json.dumps(m)[:420]); shown += 1
        print("contexts seen:", ctxs)
        print(f"updates with a timestamp: {with_ts}, without: {without_ts}\n")

async def main():
    hosts = sys.argv[1:] or [os.environ.get("SK_HOST", "127.0.0.1:3001")]
    for h in hosts:
        await show(h, h)

asyncio.run(main())
