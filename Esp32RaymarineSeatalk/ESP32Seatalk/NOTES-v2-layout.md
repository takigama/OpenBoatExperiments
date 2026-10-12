# ESP32Seatalk daughter board v2 - where it stands, and what is left

Written 2026-10-13 from the KiCad files (schematic and board saved 2026-09-28) and the AP62200 datasheet
(`datasheets/AP62200.pdf`, DS41957 Rev 2-2).

## State

- **Schematic: redone.** The AMS1117-3.3 LDO (U1, v1) is replaced by an **AP62200 buck (U3, SOT-563)**:
  L1 3.3 uH, C5 100 nF bootstrap, R3 31.6k / R4 10k feedback divider (3.33 V with the 0.8 V reference),
  C7 100 pF feedforward, C6 + C3 22 uF output, C1 input. 26 parts. ERC `pin_to_pin` is an error;
  the U3 SW / BST nets are in the Power net class.
- **Board: placed, hand-routed, DRC clean (2026-10-13).** 26 footprints (schematic parity 0 issues), 4 copper
  layers, 189 tracks, 9 vias, +3.3V plane on In1 and GND plane on In2 (filled). The buck stage is all on B.Cu.
  Outline x 76.2-111.3, y 54.9-95.0 (35.1 x 40.1 mm, unchanged from v1). KiCad DRC (with schematic parity) = 0
  violations, 0 unconnected, 0 parity issues. Pass-through copper: +12V J2.1-J1.1 1.0 mm on F.Cu (about 2-2.5 A
  at 10 C rise; bus fuse not yet confirmed), GND a 1.0 mm B.Cu loop plus the plane, HV_SEATALK 0.6 mm B.Cu.
- **Gerbers for v2 are in `jlcpcb-export-v2/`** (generated with kicad-cli 9.0.6 from the saved board, with
  `ESP32Seatalk-gerbers-JLCPCB.zip`, PTH / NPTH drill, `drc-report.json`). No BOM / pick-and-place yet (no
  LCSC part numbers in the schematic; v1 export had none either).
- v1 (the board in the photos) was fabbed from `jlcpcb-export/` (2026-09-17); that is still the only gerber set.

## Stackup: 4 layers (decided 2026-10-13)

| Layer | Use |
|---|---|
| F.Cu | components, signals, short +12V runs, GND pour |
| In1.Cu | **+3.3V** plane |
| In2.Cu | **GND** plane (swapped 2026-10-13: the buck stage is on B.Cu, so GND is the layer next to it) |
| B.Cu | components (back side), signals, GND pour |

- Interior layers are planes only: no routing on them, so the planes stay unbroken under the buck.
- +12V is **not** a plane: it is wide copper on the outer layers from J1 / J2 to C1 / C2 / U3 (pin 1) and R2.
- KiCad: Board Setup -> Board Editor Layers -> 4 copper layers; Physical Stackup -> 4 layers; add zones
  In1.Cu = +3.3V and In2.Cu = GND (whole outline); stitch GND vias around the buck and the module.
- 3.3 V plane: C6 / C3 (output caps) and the ESP32 module's 3V3 pin go to it by their own vias; L1's output
  copper lands on the output cap pads first, then the via(s) to the plane.
- Put several GND vias right at U3 pin 3 and at C1 / C2 / C6 / C3 GND pads (datasheet: plenty of vias for the
  SOT563's 110 C/W).
- JLCPCB 4-layer (1.6 mm, 1 oz outer / 0.5 oz inner), from their capabilities page: outer track / space
  0.09 / 0.09 mm; via hole 0.15 mm (0.2 preferred), via diameter 0.25 mm; annular ring 0.20 mm recommended,
  0.15 mm minimum; via-to-via 0.2 mm, pad-to-pad 0.45 mm; copper to routed edge 0.2 mm; inner-layer via hole
  to copper 0.2 mm, inner PTH pad hole to copper 0.3 mm. Check the live page before ordering.
- Design-rule floors to set (above their limits): track 0.15, clearance 0.15, via 0.5 / 0.3 (as now).

## Capacitor values (decided 2026-10-13; schematic saved 02:45 matches)

- **Input (+12V): C3 + C4, 2 x 22 uF ceramic in parallel**, 25 V (35 V if available), X5R/X7R, 0805 /
  2012 metric (Murata GRT21BR61E226ME13L is the part being considered: 22 uF / 25 V / X5R / 0805 /
  AEC-Q200, 1.45 mm tall). Nominal 44 uF, probably ~22-35 uF effective at 12-14 V. Both close to U3 pin 1 / 3,
  with **C2 (100 nF, +12V)** nearest the pins. No datasheet upper limit on the input capacitance.
- **Output (+3.3V): C1 + C6, 2 x 22 uF ceramic, 25 V, 0805.** Total 44 uF, the datasheet's 3.3 V reference
  design (range 22-68 uF effective, total).
- C5 100 nF BST-SW stays (required). The old C4 100 nF (+3.3V) is gone (C4 is now an input 22 uF).
- Cost: the 25 V 22 uF parts are about $1 each. The outputs only need >= 10 V (22 uF / 10 V is much cheaper);
  one input part is the datasheet minimum, the second is margin.
- All 22 uF parts are plain `C` symbols with `C_0805_2012Metric_Pad1.18x1.45mm_HandSolder`.
- Murata's DC-bias curve for the exact part was not seen; a typical X7R example at 50% of rating is about
  -18%, a high-density X5R 0805 can be up to ~-50%. Either way the input stays above the datasheet's 10 uF.
- Hot-plug onto the Seatalk cable: low-ESR ceramics ring (overshoot set by cable L and R, not by the amount of C);
  VIN absolute max is 20 V (22 V for 400 ms). A TVS / damping on +12V is the real fix if the supply can spike.

## To do

- [x] **L1: Cyntec VCHA042A-3R3MS6** (chosen 2026-10-13: 3.3 uH, DCR 36.8 mohm max, rated 4.1 A min,
      Isat 3.4 A min at 30% drop, 4.2 x 4.0 x 2.1 mm, AEC-Q200; DigiKey lists it as shielded; datasheet in
      `datasheets/Cyntec-VCHA042A-MS6.pdf`). Footprint `Seatalk-custom:L_Cyntec_VCHA042A-MS6` is in
      `lib/Seatalk-custom.pretty/` (project `fp-lib-table` added): pads 1.5 x 3.5 mm at +-1.85 mm, 2.2 mm gap,
      5.2 mm overall, from the datasheet's land pattern A 3.5 / B 2.2 / C 5.2 - re-check against the PDF page
      before ordering. Set it on L1 in the schematic, then "Update PCB from Schematic".
- [x] (done: all 0805 ceramic) **Capacitor footprints - switch them to ceramic MLCC.** `C1` (10 uF input) is a polarized EIA-3216
      (tantalum-style) part on a 12-14 V marine rail; the datasheet warns a tantalum there "must be surge
      protected or else capacitor failure could occur" and recommends low-ESR ceramic. Use a 10 uF ceramic
      rated >= 25 V (1206 / 0805, X7R or X5R). `C3` and `C6` (22 uF, output) are the same footprint family: the
      datasheet's reference design is 2 x 22 uF **ceramic** (10 V or more, 0805/1206). Tin-can (aluminium /
      polymer can) parts are awkward for SMD assembly - avoid them. Ceramics are unpolarised: also swap the
      three symbols from `C_Polarized` to plain `C` (same pins 1 / 2, so nets are kept) so the schematic no
      longer shows a polarity, set the footprint field, then "Update PCB from Schematic".
- [x] (done) Move the buck stage onto the board. The old LDO's spot (about 94.5, 62.5, back side) is free and next
      to the module's power pins.
- [x] Board is now 4 copper layers with the In1 +3.3V and In2 GND zones in the file (2026-10-13; since
      filled). The default physical stackup is close enough to JLCPCB's for this
      board (no impedance-controlled nets), so it is left alone. Still to do: route the outer layers and add
      GND pours + stitching vias on F.Cu / B.Cu. Power class: 0.6 mm tracks, 0.3 mm clearance
      (applies to +12V and the SW / BST nets; +3.3V and GND are planes).
- [x] (routed 2026-10-13) Layout rules from the datasheet: C1 + C2 right across pin 1 (VIN) and pin 3 (GND); L1 right at SW
      (pin 2); output caps at GND; R3 / R4 / C7 at FB (pin 6), away from SW; many GND vias round pin 3 and VIN
      vias round pin 1 (SOT563 is 110 C/W).
- [x] +12V pass-through sizing (2026-10-13): Raymarine's ST60 handbook and forum guidance fuse the SeaTalk bus at
      **5 A**; instruments draw about 45 mA each (third-party ST60+ spec sheet, 10-16 V range), so real current is
      well under 1 A. 1.0 mm outer copper = about 2.4 A at 10 C rise and about 5 A at 50 C rise (IPC-2221, 1 oz),
      fine for a 5 A fault until the fuse blows. The 2.54 mm pin headers (about 3 A per pin) are the limit, not
      the copper. Buck feed (0.4-0.6 mm) only carries the board's own load.
- [ ] Input protection: VIN is 18 V recommended, 20 V absolute (22 V for 400 ms). Consider a TVS (about
      16-18 V standoff) on the +12V input if the supply can see charging / load-dump spikes.
- [x] Project design rules set from the JLCPCB 4-layer limits (2026-10-13): min track 0.15, clearance 0.15,
      via annular ring 0.15, hole-to-hole 0.45, hole clearance 0.30, silk text 1.0 / 0.15 mm. (Net classes
      unchanged: Default 0.2, Power 0.3 / 0.6 mm; copper to edge 0.5.)
- [x] U3 pad gaps (2026-10-13): the SOT-563's pads are 0.15 mm apart, below the old Power class clearance
      (0.3 mm), so the router would not leave the pins. Net classes now: **Power 0.15 mm** (+12V, GND, +3.3V,
      SW, BST; track 0.6), **FinePitch 0.15 mm** (FB and EN nets, track 0.25), Default 0.2 mm. A custom rule
      was tried first and did not help the router; it is kept as `ESP32Seatalk.kicad_dru.disabled`.
      Pre-defined track widths 0.2 / 0.25 / 0.3 / 0.4 / 0.6 and via 0.5 / 0.3 are in the project.
- [x] DRC clean and fresh gerbers into `jlcpcb-export-v2/` (2026-10-13). Still open: ERC on the schematic,
      BOM / pick-and-place only if JLCPCB assembly is wanted.
- [ ] README: the photos section says "the board in the photos is v1" - update when v2 exists.

## Checked

- AP62200 SOT563 pin map (1 VIN, 2 SW, 3 GND, 4 BST, 5 EN, 6 FB) matches U3 as wired.
- EN left unconnected is allowed ("can be left open for automatic startup").
- Divider, inductor value, output caps and bootstrap cap match the datasheet's 3.3 V table.
