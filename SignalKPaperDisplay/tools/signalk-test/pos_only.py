#!/usr/bin/env python3
"""Publish ONLY navigation.position (nothing else) to a SignalK server, moving at about 5 kn toward 055 deg.
usage: pos_only.py [host:port] [lat lon]"""
import os
import asyncio, json, math, sys, time
import websockets

HOST = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("SK_HOST", "127.0.0.1:3001")
lat = float(sys.argv[2]) if len(sys.argv) > 3 else -28.76832
lon = float(sys.argv[3]) if len(sys.argv) > 3 else 157.23132
KN, COG = 5.0, math.radians(55)

async def main():
    global lat, lon
    print(f"publishing only navigation.position to {HOST}, from {lat:.5f}, {lon:.5f}", flush=True)
    while True:
        try:
            async with websockets.connect(f"ws://{HOST}/signalk/v1/stream?subscribe=none", ping_interval=None) as ws:
                await ws.recv()
                while True:
                    d_nm = KN / 3600.0                      # one second of travel
                    lat += d_nm * math.cos(COG) / 60.0
                    lon += d_nm * math.sin(COG) / (60.0 * math.cos(math.radians(lat)))
                    await ws.send(json.dumps({"context": "vessels.self", "updates": [{"$source": "pos-only",
                        "values": [{"path": "navigation.position", "value": {"latitude": lat, "longitude": lon}}]}]}))
                    await asyncio.sleep(1)
        except Exception as e:
            print("reconnecting:", type(e).__name__, flush=True)
            await asyncio.sleep(2)

asyncio.run(main())
