# kindle-pw3

Kindle Paperwhite 3 (7th gen), firmware 5.14.3.0.1, jailbroken.

Facts gathered from a real unit over SSH (Dropbear on port 2223):

| | |
|---|---|
| SoC / RAM | i.MX6 SoloLite (ARMv7), 512 MB |
| Screen | 1072x1448, 8-bit framebuffer, 16 gray levels |
| Touch | `cyttsp4_mt` on `/dev/input/event1`, type-B multitouch (slots) |
| Power button | `max77696-onkey` on `/dev/input/event0` |
| On device | `curl`, `lua`, `eips`, `nc`, busybox, `lipc-*`, `stop`/`start`. No Python, no FBInk |
| User storage | `/mnt/us` (FAT-style, ~2.8 GB free). Root fs has only ~50 MB free, so install nothing there |

## Still to verify on the device

- That `stop framework` leaves the screen free to draw on (the stock UI
  processes are `awesome`, `blanket`, `powerd`).
- A prebuilt Kindle FBInk binary on `/mnt/us` actually runs here.
- Keeping the screen awake while plugged in:
  `lipc-set-prop com.lab126.powerd preventScreenSaver 1` (resets on reboot,
  so the launcher script sets it on every start).

## Build

```
make kindle-pw3        # -> dist/kindle-pw3/paperdisplay (static ARMv7)
```
