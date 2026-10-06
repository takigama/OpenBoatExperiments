#!/usr/bin/env python3
"""Read-only: how does a real SignalK server treat the idle subscription?
Connect with subscribe=none, subscribe to ONE path, listen; also ping."""
import asyncio, json, os, sys, time
import websockets

async def probe(host, path, listen=6):
    url = f"ws://{host}/signalk/v1/stream?subscribe=none"
    print(f"\n=== {host}  path {path}")
    try:
        async with websockets.connect(url, open_timeout=6, ping_interval=None) as ws:
            t0 = time.time()
            hello = await asyncio.wait_for(ws.recv(), 5)
            h = json.loads(hello)
            print(f"hello after {time.time()-t0:.2f}s: name={h.get('name')} version={h.get('version')} self={h.get('self')}")
            await ws.send(json.dumps({"context": "vessels.self", "subscribe": [{"path": path, "policy": "instant"}]}))
            sent = time.time()
            got = []
            pong = None
            try:
                pong_waiter = await ws.ping()
                p0 = time.time()
            except Exception as e:
                pong_waiter = None
            end = time.time() + listen
            while time.time() < end:
                try:
                    msg = await asyncio.wait_for(ws.recv(), end - time.time())
                except asyncio.TimeoutError:
                    break
                d = json.loads(msg)
                for u in d.get("updates", []):
                    for v in u.get("values", []):
                        got.append((round(time.time() - sent, 2), d.get("context"), v.get("path"), v.get("value")))
            if pong_waiter is not None:
                try:
                    await asyncio.wait_for(pong_waiter, 1)
                    pong = f"{time.time()-p0:.2f}s"
                except Exception as e:
                    pong = f"none ({type(e).__name__})"
            print(f"ping answered: {pong}")
            print(f"{len(got)} value(s) in {listen}s after subscribing to just that path:")
            for g in got[:6]:
                print("   ", g)
            only = {g[2] for g in got}
            print("only the one path:", only <= {path})
    except Exception as e:
        print("failed:", type(e).__name__, e)

async def main():
    # usage: probe_sk.py [host:port ...] [-p path]   (default host from SK_HOST)
    args = sys.argv[1:]
    path = "navigation.speedOverGround"
    if "-p" in args:
        i = args.index("-p"); path = args[i + 1]; del args[i:i + 2]
    for host in (args or [os.environ.get("SK_HOST", "127.0.0.1:3001")]):
        await probe(host, path)

asyncio.run(main())
