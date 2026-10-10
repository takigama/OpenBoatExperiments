/**
 * board_select.h - pick which physical display board this build targets.
 *
 * Included first by engine_display.ino, esp_panel_board_supported_conf.h,
 * and esp_panel_board_custom_conf.h, so it must stay dependency-free (no
 * other project headers) and safe to include multiple times via #pragma
 * once. Edit ONLY the TARGET_BOARD line below, then reflash.
 */
#pragma once

#define BOARD_HELM_S3_800x480   1   /* VIEWE UEDX80480070E-WB-A, 800x480 RGB, ESP32-S3-N16R8 - the real HELM panel */
#define BOARD_CYD_28_RESISTIVE  2   /* "CYD" 2.8" ILI9341 SPI 320x240, XPT2046 resistive touch (separate SPI bus), plain ESP32 */
#define BOARD_CYD_28_CAPACITIVE 3   /* "CYD" 2.8" ILI9341 SPI 320x240, GT911 capacitive touch (I2C), plain ESP32 */
#define BOARD_CYD_24_RESISTIVE  4   /* "CYD" 2.4" ILI9341 SPI 320x240, XPT2046 resistive touch, different backlight pin than the 2.8" boards */

/* tools/devices.json builds each target by passing -DTARGET_BOARD=<n> to the
 * compiler, so this default only applies to a build made by hand (Arduino IDE). */
#ifndef TARGET_BOARD
#define TARGET_BOARD  BOARD_HELM_S3_800x480   /* <-- EDIT THIS LINE, then reflash */
#endif
