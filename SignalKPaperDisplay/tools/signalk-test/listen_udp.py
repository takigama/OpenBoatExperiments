#!/usr/bin/env python3
"""Read-only: listen for broadcast/multicast NMEA 0183 on the usual UDP ports and decode any position."""
import select, socket, struct, sys, time

PORTS = [10110, 2000, 4352, 5000, 5001, 6000, 10111, 1456, 3000, 8000, 2947, 10100, 4123, 7000, 7777, 8888, 9000]
SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 30

def deg(v, hemi):
    if not v:
        return None
    dot = v.index(".")
    d, m = float(v[: dot - 2]), float(v[dot - 2:])
    r = d + m / 60
    return -r if hemi in ("S", "W") else r

def decode(line):
    f = line.split("*")[0].split(",")
    t = f[0][-3:]
    try:
        if t == "GGA" and f[2]:
            return deg(f[2], f[3]), deg(f[4], f[5]), "GGA fix quality " + f[6]
        if t == "RMC" and f[3]:
            return deg(f[3], f[4]), deg(f[5], f[6]), "RMC status " + f[2]
        if t == "GLL" and f[1]:
            return deg(f[1], f[2]), deg(f[3], f[4]), "GLL"
    except Exception:
        return None

socks = {}
bound, busy = [], []
for p in PORTS:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except Exception:
        pass
    try:
        s.bind(("0.0.0.0", p))
        socks[s] = p
        bound.append(p)
    except OSError as e:
        busy.append((p, str(e)))
print("listening on UDP ports:", bound)
if busy:
    print("could not bind:", busy)
# also join the common multicast groups some gateways use
for p in (10110,):
    try:
        for s, port in socks.items():
            if port == p:
                s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, struct.pack("4s4s", socket.inet_aton("239.192.0.4"), socket.inet_aton("0.0.0.0")))
    except Exception as e:
        print("multicast join skipped:", e)

end = time.time() + SECS
got = {}
while time.time() < end:
    r, _, _ = select.select(list(socks), [], [], 0.5)
    for s in r:
        data, addr = s.recvfrom(4096)
        txt = data.decode("latin-1", "replace").strip()
        key = (addr[0], socks[s])
        got.setdefault(key, []).append(txt)
        for line in txt.splitlines():
            pos = decode(line) if line.startswith("$") else None
            tag = f"   -> POSITION {pos[0]:.4f}, {pos[1]:.4f} ({pos[2]})" if pos and pos[0] is not None else ""
            if len(got[key]) <= 3 or tag:
                print(f"  from {addr[0]}:{addr[1]} on port {socks[s]}: {line[:90]}{tag}", flush=True)
print(f"\nin {SECS} s: " + (", ".join(f"{len(v)} datagram(s) from {k[0]} on port {k[1]}" for k, v in got.items()) if got else "nothing arrived on any of those ports"))
