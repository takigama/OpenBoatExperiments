import os
import asyncio, json, math, time
import websockets

H = os.environ.get("SK_HOST", "127.0.0.1:3001")

def dist_nm(a, b):
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    d = 2 * math.asin(math.sqrt(math.sin((la2 - la1) / 2) ** 2 + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2))
    return d * 3440.065

def bearing(a, b):
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    y = math.sin(lo2 - lo1) * math.cos(la2)
    x = math.cos(la1) * math.sin(la2) - math.sin(la1) * math.cos(la2) * math.cos(lo2 - lo1)
    return (math.degrees(math.atan2(y, x)) + 360) % 360

async def main():
    pos, sog, cog, hdg = [], None, None, None
    async with websockets.connect(f"ws://{H}/signalk/v1/stream?subscribe=all", ping_interval=None) as ws:
        hello = json.loads(await ws.recv()); self_id = hello["self"]
        t0 = time.time()
        while time.time() - t0 < 20:
            try:
                m = json.loads(await asyncio.wait_for(ws.recv(), 20 - (time.time() - t0)))
            except asyncio.TimeoutError:
                break
            if m.get("context") not in ("vessels.self", self_id):
                continue
            for u in m.get("updates", []):
                if (u.get("$source") or "") != "sim.XX":
                    continue
                for v in u.get("values", []):
                    if v["path"] == "navigation.position":
                        pos.append((time.time(), v["value"]["latitude"], v["value"]["longitude"]))
                    elif v["path"] == "navigation.speedOverGround": sog = v["value"]
                    elif v["path"] == "navigation.courseOverGroundTrue": cog = v["value"]
                    elif v["path"] == "navigation.headingTrue": hdg = v["value"]
    print(f"{len(pos)} position updates in 20 s from the simulator (source sim.XX)")
    if len(pos) < 2:
        return
    a, b = pos[0], pos[-1]
    secs = b[0] - a[0]
    d = dist_nm((a[1], a[2]), (b[1], b[2]))
    print(f"first position: {a[1]:.4f}, {a[2]:.4f}")
    print(f"last position:  {b[1]:.4f}, {b[2]:.4f}")
    print(f"it moved {d:.2f} nm in {secs:.1f} s  =  {d/secs*3600:.0f} knots actually travelled")
    print(f"reported SOG: {sog/0.514444:.1f} kn   COG {math.degrees(cog):.0f} deg   heading {math.degrees(hdg):.0f} deg" if sog is not None and cog is not None and hdg is not None else "SOG/COG not seen")
    print(f"bearing it actually moved on: {bearing((a[1], a[2]), (b[1], b[2])):.0f} deg")
    steps = [dist_nm((pos[i][1], pos[i][2]), (pos[i + 1][1], pos[i + 1][2])) for i in range(len(pos) - 1)]
    print(f"step per update: min {min(steps)*1852:.0f} m, max {max(steps)*1852:.0f} m  (updates every {secs/(len(pos)-1):.2f} s)")

asyncio.run(main())
