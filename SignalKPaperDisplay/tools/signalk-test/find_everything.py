import json, os, sys, urllib.request, urllib.error

def get(host, path):
    try:
        with urllib.request.urlopen(f"http://{host}{path}", timeout=6) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, None
    except Exception as e:
        return None, str(e)

def walk(node, path, out):
    if isinstance(node, dict):
        if "latitude" in node and "longitude" in node and isinstance(node["latitude"], (int, float)):
            out.append((path, node["latitude"], node["longitude"]))
        if "coordinates" in node and isinstance(node["coordinates"], list) and len(node["coordinates"]) >= 2 and isinstance(node["coordinates"][0], (int, float)):
            out.append((path + ".coordinates(lon,lat)", node["coordinates"][1], node["coordinates"][0]))
        for k, v in node.items():
            walk(v, path + "." + k if path else k, out)
    elif isinstance(node, list):
        for i, v in enumerate(node):
            walk(v, f"{path}[{i}]", out)

def near(la, lo):
    return 15 <= la <= 23 and 23 <= lo <= 31

for host in (sys.argv[1:] or [os.environ.get("SK_HOST", "127.0.0.1:3001")]):
    name = host
    print(f"=== {name}")
    code, root = get(host, "/signalk/v1/api/")
    print("   top-level sections of the data model:", sorted(root.keys()) if isinstance(root, dict) else code)
    found = []
    if isinstance(root, dict):
        walk(root, "", found)
    print(f"   positions anywhere in the whole model: {len(found)}")
    for p, la, lo in found:
        print(f"     {'NEAR 19N27E ' if near(la, lo) else ''}{p[:75]}  {la:.3f}, {lo:.3f}")
    for path in ("/signalk/v1/api/resources/waypoints", "/signalk/v1/api/resources/routes", "/signalk/v1/api/resources/regions",
                 "/signalk/v1/api/resources/notes", "/signalk/v2/api/vessels/self/navigation/course", "/signalk/v2/api/resources/waypoints",
                 "/signalk/v1/api/atons", "/signalk/v1/api/sources"):
        code, data = get(host, path)
        extra = ""
        if isinstance(data, (dict, list)):
            pos = []
            walk(data, "", pos)
            extra = f"  {len(pos)} position(s)" + ("".join(f"\n        {'NEAR ' if near(la, lo) else ''}{p[:60]}  {la:.3f}, {lo:.3f}" for p, la, lo in pos[:6]) if pos else "")
            if not pos:
                extra += "  " + json.dumps(data)[:110]
        print(f"   {path:<52} -> {code}{extra}")
