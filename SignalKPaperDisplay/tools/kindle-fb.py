#!/usr/bin/env python3
"""Grab the 8th-gen Kindle's screen (read /dev/fb0 over key-only ssh) into a PNG.
usage: kindle-fb.py out.png [host]   (key-only ssh on port 2223)   (8-bit, stride 608, 600x800 visible)"""
import os, subprocess, sys, struct, zlib

out = sys.argv[1]
host = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("KINDLE_HOST")
if not host:
    sys.exit("give the Kindle's address: kindle-fb.py out.png HOST (or set KINDLE_HOST)")
raw = subprocess.run(["ssh", "-p", "2223", "-o", "BatchMode=yes", "-o", "PasswordAuthentication=no",
                      "-o", "KbdInteractiveAuthentication=no", "-o", "StrictHostKeyChecking=no", "-o", "ConnectTimeout=8",
                      f"root@{host}", "dd if=/dev/fb0 bs=608 count=800 2>/dev/null"], capture_output=True, timeout=60).stdout
if len(raw) != 608 * 800:
    sys.exit(f"got {len(raw)} bytes, wanted {608*800}")
w, h, stride = 600, 800, 608
rows = b"".join(b"\x00" + raw[y * stride:y * stride + w] for y in range(h))

def chunk(t, d):
    c = struct.pack(">I", len(d)) + t + d
    return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
print("wrote", out)
