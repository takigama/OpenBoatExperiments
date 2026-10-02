# kindle-basic

A Kindle Basic: touchscreen, **no front light**, 600x800, firmware 5.15.1,
jailbroken. Facts from a real unit over SSH (Dropbear on port 2223):

| | |
|---|---|
| SoC / RAM | i.MX6 SoloLite, ARMv7 with VFP and NEON (the same build as the Paperwhite 3), 512 MB |
| Screen | 600x800, 8-bit framebuffer (stride 608, `rotate` reads 3) |
| Touch | `zforce2` on `/dev/input/event0`, type-B multitouch (slots); no hardware buttons |
| Power | battery `bd7181x_bat`, chargers `bd7181x_ac` and `imx6_usb_charger` |
| Front light | none (the `flIntensity` properties exist in every Kindle's firmware, but there is no backlight device) |
| On device | `curl`, `lua`, `eips`, `nc`, busybox, `lipc-*`, `stop`/`start`. No Python, no FBInk |

Serial prefix `G000`. The profile's name is a guess at the model; correct it if it
turns out to be something more specific.

## How the layout fits

Every page is laid out in a 1072-wide "design" space (the Paperwhite 3's width)
and drawn by `render.Canvas`, which scales positions, text and line weights to the
real screen. So this device needs no pages of its own: it is the Paperwhite's
layout at 56% size. Touches are converted back to design units before they are
matched against tap areas.

## Still to verify on the device

- **Orientation.** `fb0/rotate` is 3 and the framebuffer is 608 wide; FBInk should
  cope, but until it has been seen drawing, an upside-down or sideways picture is
  possible. The profile's `rotation` field is not used by any code yet.
- **Touch axes.** Measure with `./paperdisplay -touch-test` and record `swapXY`,
  `invertX` and `invertY` in `profile.json` if the mapping isn't straight.
- **The stock UI.** Found with `initctl list`: the Java UI is the `framework`
  job and the status bar (`JunoStatusBarDriver`) is a job of its own, `statusbar`.
  `launcher.conf.example` here sets `STOP_JOBS="framework statusbar"`; `pillow` and
  `webreader` are also running and may need adding if something paints over us.
- **FBInk.** The `kindlepw2` build we ship should run here (KOReader uses it for
  every Kindle from the Paperwhite 2 on).

## Install

```
bash platforms/kindle-basic/install/deploy.sh <ip-address> [port]
```

copies this device's binary, its `profile.json` (so it must be run for this
Kindle, not the Paperwhite - a Paperwhite profile makes it open `event1` and draw
at the wrong size) and the shared launcher scripts. Then, on the device,
`/mnt/us/signalk/launcher.conf` is where the settings live: copy
`launcher.conf.example` (shipped beside it) to `launcher.conf` and set
`SIGNALK_HOST`.
