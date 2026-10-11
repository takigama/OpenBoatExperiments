A really simple board for connecting an ESP32-C3-OLED board to seatalk as an in-path or end-of-path device.

Also has support for CAN BUS (for NEMA2000) and UART extension (which will be useful for the project revolving around using google tags as MOB tokens)

Firmware (SeaTalk/CAN/MQTT/SignalK bridge) lives in [`PlatformIO/ESP32Seatalk`](PlatformIO/ESP32Seatalk/README.md) - see that README for the MQTT topic mapping, web UI, and everything else.

## The built board

A bare board and the same board built up: an ESP32-C3 module with its small OLED sits on the carrier board
(its small display shows what the firmware is doing); the SeaTalk connectors are on the two sides, and the
voltage regulator (U1) with its capacitors, the BSS138 level shifter (Q1) for the one-wire SeaTalk line and
two resistors (R1, R2) are on the back.

| Bare PCB, component side | Bare PCB, back |
|---|---|
| <img src="ESP32Seatalk/Images/20260922_123025.jpg" alt="Bare PCB, both sides, as received" width="420"> | <img src="ESP32Seatalk/Images/20260922_123145.jpg" alt="Bare PCB, both sides, second photo" width="420"> |

| Built: front | Built: back |
|---|---|
| <img src="ESP32Seatalk/Images/made_front_of_board.jpg" alt="Front of the built board: ESP32-C3 OLED module on the carrier, a 3-pin SeaTalk connector each side" width="400"> | <img src="ESP32Seatalk/Images/made_back_of_board.jpg" alt="Back of the built board: U1 regulator, C1-C4, Q1, R1, R2" width="400"> |

The two white 3-pin connectors (J1 and J2, the red / yellow / black leads) are SeaTalk: +12 V, data and ground.
The board takes its power from the bus, so it sits in the path of the bus or at its end. The 5-way header at the
top (J4) is where a plug-in CAN transceiver module goes, and the transceiver's CANH / CANL leave on connector
J3; neither is fitted in these photos.

### With its cables

The board with its two SeaTalk lead cables (ferrite sleeves, round screw-lock connectors), and **no NMEA 2000
transceiver fitted**: SeaTalk only.

<img src="ESP32Seatalk/Images/made_board_and_cables.jpg" alt="The board with its two SeaTalk cables; no NMEA2000 transceiver fitted" width="330">

### Driving a real instrument

The board connected to a Raymarine **ST60+ Wind** (Autohelm series) instrument on a bench SeaTalk bus. The
firmware's demo mode sends three fixed values - speed through water, apparent wind angle and apparent wind
speed - and the instrument works out the true wind and the VMG itself. SeaTalk counts the wind angle 0-360 deg
clockwise from the bow, so a wind 60 deg off the port bow is sent as 300 deg.

| Photo | Speed through water | Apparent wind angle sent | Apparent wind speed | The instrument shows |
|---|---|---|---|---|
| APP mode | 10 kn | **315 deg** (45 deg to port) | 10 kn | apparent wind speed **10.0 kts**, needle 45 deg to port |
| TRUE mode | 10 kn | **300 deg** (60 deg to port) | 10 kn | true wind speed **10.0 kts**, true wind angle **120 deg** to port |
| VMG mode | 10 kn | **300 deg** (60 deg to port) | 10 kn | VMG **-5.0 kts**, needle 120 deg to port |

| ST60+ in APP mode | in TRUE mode | in VMG mode |
|---|---|---|
| <img src="ESP32Seatalk/Images/made_board_driving_st60wind_app.jpg" alt="ST60+ Wind in apparent mode: 10.0 kts, needle 45 degrees to port" width="260"> | <img src="ESP32Seatalk/Images/made_board_driving_st60wind_true.jpg" alt="ST60+ Wind in true mode: 10.0 kts, true wind angle 120 degrees to port" width="260"> | <img src="ESP32Seatalk/Images/made_board_driving_st60wind_vmg.jpg" alt="ST60+ Wind in VMG mode: -5.0 kts" width="260"> |
| 45 deg to port, 10.0 kts | 120 deg to port, 10.0 kts | VMG -5.0 kts |

The instrument's numbers agree with the vector calculation. For the 60 deg photos (TRUE and VMG): true wind
angle atan2(10 sin 60, 10 cos 60 - 10) = **120 deg**, still on the port side, true wind speed
sqrt(10^2 + 10^2 - 2 x 10 x 10 x cos 60) = **10 kn**, and VMG to the wind = STW x cos(TWA) = 10 x cos 120 =
**-5.0 kn** (negative: making progress away from the wind). The APP photo was taken at a different angle, so it
only shows the apparent wind - for reference, 45 deg would have given a true wind angle of 112.5 deg, a true
wind speed of 7.7 kn and a VMG of -3.8 kn.

The needle sitting on the red (port) side of the dial for sent angles of 300 and 315 deg is also a check on the
firmware's encoding of the wind angle: high byte first, over the full 0-360 deg range.
