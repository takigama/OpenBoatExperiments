#!/usr/bin/env python3
"""Relay the live stream of one SignalK server onto another, but only the paths listed in a control file,
which is re-read as it changes: {"paths": ["navigation.position", ...], "others": false}
'others' lets the other vessels (AIS contexts) through. usage: relay_ctl.py src dst controlfile"""
import asyncio, json, os, sys
import websockets

SRC, DST, CTL = sys.argv[1], sys.argv[2], sys.argv[3]
state = {"mtime": 0, "paths": set(), "others": False}

def reload():
    try:
        m = os.stat(CTL).st_mtime
        if m != state["mtime"]:
            d = json.load(open(CTL))
            state["paths"], state["others"], state["mtime"] = set(d.get("paths", [])), bool(d.get("others")), m
    except Exception:
        pass

async def main():
    async with websockets.connect(f"ws://{SRC}/signalk/v1/stream?subscribe=all", ping_interval=None) as s, \
               websockets.connect(f"ws://{DST}/signalk/v1/stream?subscribe=none", ping_interval=None) as d:
        hello = json.loads(await s.recv()); self_id = hello["self"]; await d.recv()
        print(f"relaying {SRC} -> {DST}, controlled by {CTL}", flush=True)
        async for raw in s:
            m = json.loads(raw)
            if "updates" not in m:
                continue
            reload()
            ctx = m.get("context", "")
            own = ctx in ("vessels.self", self_id)
            if not own and not state["others"]:
                continue
            ups = []
            for u in m["updates"]:
                vals = [v for v in u.get("values", []) if (not own) or "*" in state["paths"] or v["path"] in state["paths"]]
                if vals:
                    nu = {k: v for k, v in u.items() if k != "values"}   # keep source, timestamp, ... exactly as sent
                    if "source" in nu:
                        nu.pop("$source", None)                          # the server derives $source from the source object
                    nu["values"] = vals
                    ups.append(nu)
            if ups:
                await d.send(json.dumps({"context": "vessels.self" if own else ctx, "updates": ups}))

asyncio.run(main())
