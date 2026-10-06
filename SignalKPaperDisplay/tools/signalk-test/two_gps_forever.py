#!/usr/bin/env python3
"""Publish two GPS sources at the same time, constantly, until killed.
Each sends position, COG and SOG once a second under its own $source label."""
import os
import asyncio, json, math, sys, time
import websockets

HOST = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("SK_HOST", "127.0.0.1:3001")
KN = 0.514444
SRC = {
    "A": {"label": "test-gps-A", "lat": -33.8450, "lon": 151.2400, "cog": math.pi / 2, "sog": 5 * KN},
    "B": {"label": "test-gps-B", "lat": -33.8450 - 5.0 / 60, "lon": 151.2400, "cog": 3 * math.pi / 2, "sog": 8 * KN},
}

def msg(s):
    vals = [
        {"path": "navigation.position", "value": {"latitude": s["lat"], "longitude": s["lon"]}},
        {"path": "navigation.courseOverGroundTrue", "value": s["cog"]},
        {"path": "navigation.speedOverGround", "value": s["sog"]},
    ]
    return json.dumps({"context": "vessels.self", "updates": [{"$source": s["label"], "values": vals}]})

async def main():
    print(f"publishing A and B constantly to {HOST} (kill me to stop)", flush=True)
    while True:
        try:
            async with websockets.connect(f"ws://{HOST}/signalk/v1/stream?subscribe=none", ping_interval=None) as ws:
                await ws.recv()
                while True:
                    keys = sys.argv[2] if len(sys.argv) > 2 else "AB"   # e.g. "B" to publish only B
                    for key in keys:
                        await ws.send(msg(SRC[key]))
                        await asyncio.sleep(1.0 / len(keys))
        except Exception as e:
            print("reconnecting:", type(e).__name__, flush=True)
            await asyncio.sleep(2)

asyncio.run(main())
