# kindle-basic

The Kindle (8th generation, 2016), sold as the plain "Kindle": touchscreen,
**no front light**, 600x800, firmware 5.15.1, jailbroken. Facts from a real unit over SSH (Dropbear on port 2223):

| | |
|---|---|
| SoC / RAM | i.MX6 SoloLite, ARMv7 with VFP and NEON (the same build as the Paperwhite 3), 512 MB |
| Screen | 600x800, 8-bit framebuffer (stride 608, `rotate` reads 3) |
| Touch | `zforce2` on `/dev/input/event0`, type-B multitouch (slots); no hardware buttons |
| Power | battery `bd7181x_bat`, chargers `bd7181x_ac` and `imx6_usb_charger` |
| Front light | none (the `flIntensity` properties exist in every Kindle's firmware, but there is no backlight device) |
| On device | `curl`, `lua`, `eips`, `nc`, busybox, `lipc-*`, `stop`/`start`. No Python, no FBInk |

Serial prefix `G000`; the device itself says "Kindle 8th generation". The platform
is called kindle-basic because other plain Kindles of this era (no light) should
be able to share it.

## How the layout fits

Every page is laid out in a 1072-wide "design" space (the Paperwhite 3's width)
and drawn by `render.Canvas`, which scales positions, text and line weights to the
real screen. So this device needs no pages of its own: it is the Paperwhite's
layout at 56% size. Touches are converted back to design units before they are
matched against tap areas.

## Verified on the device

- **Orientation and drawing.** FBInk identifies it as a Kindle Basic 2 (`Eanab`,
  Heisenberg), 600x800 at 167 dpi, and copes with the rotated framebuffer: the
  picture is upright (read back from `/dev/fb0`), and a full refresh takes about
  0.57 s (render 53 ms, FBInk 500 ms).
- **Touch.** `event0` maps straight to the screen with no `swapXY` or inversion:
  the cog opens settings, the left and right thirds change page, and the compass
  page's wind widget and speed box respond.
- **The stock UI.** The Java UI is the `framework` job and the status bar
  (`JunoStatusBarDriver`) is a job of its own, `statusbar`. `launcher.conf.example`
  here sets `STOP_JOBS="framework statusbar"`, which leaves the screen to us.
  `pillow` and `webreader` also keep running and have not been a problem.
- **Battery.** `bd7181x_bat` is read through sysfs like the Paperwhite's.
- **FBInk.** The shared `kindlepw2` build runs here.

## Not applicable

- No front light, so the Backlight setting is hidden (the profile declares none).

## Install

```
bash platforms/kindle-basic/install/deploy.sh <ip-address> [port] --signalk <host:port>
```

copies this device's binary, its `profile.json` (so it must be run for this
Kindle, not the Paperwhite - a Paperwhite profile makes it open `event1` and draw
at the wrong size), FBInk and the launcher; writes `/mnt/us/signalk/launcher.conf`
from the example here (with your SignalK server); adds the launcher to the
crontab; starts it; and shows the log. Run it again to update the files.
`bash platforms/kindle-basic/install/uninstall.sh <ip-address> [port]` takes it
off again and gives the Kindle back to the stock software.
