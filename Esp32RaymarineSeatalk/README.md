A really simple board for connecting an ESP32-C3-OLED board to seatalk as an in-path or end-of-path device.

Also has support for CAN BUS (for NEMA2000) and UART extension (which will be useful for the project revolving around using google tags as MOB tokens)

Firmware (SeaTalk/CAN/MQTT/SignalK bridge) lives in [`PlatformIO/ESP32Seatalk`](PlatformIO/ESP32Seatalk/README.md) - see that README for the MQTT topic mapping, web UI, and everything else.
