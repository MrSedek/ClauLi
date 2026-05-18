#pragma once

#include <Arduino_GFX_Library.h>
#include <Wire.h>

// ---- Display resolution ----
#define LCD_WIDTH   240
#define LCD_HEIGHT  280

// ---- SPI display pins (ST7789V2) ----
// Per-board GPIOs come from platformio.ini build_flags (-DPIN_LCD_*). The
// defaults below are the verified NanoESP32-C6 wiring; other boards override
// every pin via build_flags (see README pin tables — adjust to your wiring).
// C6 note: GPIO 12-15,19-22 = SPI flash (LOCKED); only HP GPIO 10,23 are fast.
#ifndef PIN_LCD_MOSI
#define PIN_LCD_MOSI    10
#endif
#ifndef PIN_LCD_SCLK
#define PIN_LCD_SCLK    23
#endif
#ifndef PIN_LCD_CS
#define PIN_LCD_CS      5
#endif
#ifndef PIN_LCD_DC
#define PIN_LCD_DC      4
#endif
#ifndef PIN_LCD_RESET
#define PIN_LCD_RESET   6
#endif
#ifndef PIN_LCD_BL
#define PIN_LCD_BL      7
#endif
#ifndef PIN_BTN_BOOT
#define PIN_BTN_BOOT    9
#endif
#ifndef PIN_BTN_A
#define PIN_BTN_A       7
#endif
#ifndef PIN_BTN_B
#define PIN_BTN_B       6
#endif

#define LCD_MOSI    PIN_LCD_MOSI   // DIN
#define LCD_SCLK    PIN_LCD_SCLK   // CLK
#define LCD_CS      PIN_LCD_CS
#define LCD_DC      PIN_LCD_DC
#define LCD_RESET   PIN_LCD_RESET
#define LCD_BL      PIN_LCD_BL     // Backlight (PWM-capable)

// ST7789V2 offsets for 240x280 module in 240x320 controller
#define LCD_COL_OFFSET  0
#define LCD_ROW_OFFSET  20

// ---- Buttons ----
#define BTN_BOOT     PIN_BTN_BOOT  // onboard BOOT button

// ---- External buttons (optional — set active=false if not connected) ----
#define BTN_A        PIN_BTN_A     // external left button
#define BTN_B        PIN_BTN_B     // external right button

// ---- Button timing (ms) ----
#define BTN_SHORT_MS    300     // max duration for "short press"
#define BTN_LONG_MS     800     // min duration for "long press"
#define BTN_DOUBLE_MS   400     // max gap between taps for double press

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus *bus;
extern Arduino_ST7789 *gfx;

// ---- Current rotation (managed in main.cpp) ----
extern uint8_t current_rotation;
