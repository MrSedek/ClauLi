# PLAN — ClauLi (ESP32-C6-Zero + 1.69" LCD ST7789V2)

## Goal
ESP32-C6-Zero firmware + macOS BLE daemon + Web UI for Claude Code usage tracking.

## Key Constraints
- **No PSRAM** — all allocations in internal SRAM (~250-300KB heap after BLE stack)
- **No touch** — LCD module has no touch
- **No PMU/IMU** — battery/accelerometer removed
- **Single button** — BOOT (GPIO9) with multi-press patterns
- **Smaller display** — 240×280 portrait
- **Flash at 96.2%** — splash_animations.h is 180KB (13 anims); must trim to fit new features
- **LVGL 9 font patching** — lv_font_conv outputs LVGL 8 format; must patch .cache→.release_glyph etc.
- **Cyrillic fonts** — built-in Montserrat has no Cyrillic; custom _cyr fonts generated but build blocked by LVGL symbol resolution

## Completed Phases

### Phase 1: Core infrastructure ✅
- [x] Create docs/ structure and PLAN.md
- [x] Add env esp32_c6_zero to platformio.ini
- [x] Rewrite display_cfg.h for C6 + ST7789V2 SPI pins
- [x] Rewrite main.cpp — remove PSRAM/touch/IMU/PMU, add BOOT button handler

### Phase 2: Remove dead modules ✅
- [x] Delete power.cpp/h (AXP2101)
- [x] Delete imu.cpp/h (QMI8658)

### Phase 3: UI rework ✅
- [x] Rewrite splash.cpp/h — direct GFX rendering, no canvas buffer
- [x] Rewrite ui.cpp/h — 240×280 layout, remove battery widget, remove touch callbacks
- [x] Use LVGL built-in Montserrat fonts (temporary until custom fonts regenerated)

### Phase 4: BLE + polish ✅
- [x] Verify ble.cpp for ESP32-C6 compatibility (works — NimBLE-Arduino supports C6)
- [x] Build test and fix compilation errors — **BUILD SUCCEEDS**

### Phase 5: Extended features ✅
- [x] BLE CTRL characteristic (0x01-0x05, 0x10)
- [x] Hero screen (procedural animated character with 5 expressions)
- [x] Russian-language UI text
- [x] Web UI with SVG arc gauges, WebSocket
- [x] Daemon with HTTP API + aiohttp server
- [x] CLI mode via claude-ctrl.sh
- [x] Custom Cyrillic Montserrat fonts generated (bpp=1, 149 chars, 144KB total)

## Completed: Phase 6 ✅

### Phase 6: EMO hero + Web controls + Layout fix
- [x] **6.1** Fix LVGL font symbol resolution (build blocker)
  - Regenerated 4 Cyrillic fonts fresh with lv_font_conv; wrote definitive LVGL9 patcher script
  - Added `CONFIG_LV_FONT_DEFAULT_MONTSERRAT_10`, `LV_LVGL_H_INCLUDE_SIMPLE`, `extra_scripts = font_custom_declare.py` to esp32_c6_zero env
  - Removed redundant `extern lv_font_t` declarations from hero.cpp and emo.cpp
  - Font symbols declared `const` to match `LV_FONT_DECLARE` extern
  - Build: **87.3% flash**, SUCCESS
- [x] **6.2** Splash animations already at 6 (was done previously)
- [x] **6.3** emo.cpp/emo.h already created; fixed `lv_obj_clear_flag` OR-combining and `case` variable scoping
- [x] **6.4** Added SCREEN_EMO to ui.h; wired into ui.cpp (init/show/hide/tick/set_usage/next_mood), main.cpp (CTRL 0x06), splash.cpp (splash_set_anim)
  - Screen cycle: Usage → Hero → EMO → Splash → Usage
- [x] **6.5** Web UI: added EMO button, animation dropdown (6 slots), color dropdown (6 presets)
- [x] **6.6** Daemon: added `POST /api/animation` (→ CTRL 0x20+idx) and `POST /api/color` (→ CTRL 0x30+id); added CTRL_SCREEN_EMO = 0x06
- [x] **6.7** Usage screen layout: session% moved up, weekly% increased font to montserrat_16, spacing increased 8px

## Next Steps / Future Work

## Button Map (GPIO9 / BOOT)
| Pattern | Action |
|---|---|
| Short press (<300ms) | Trigger animation change on current screen |
| Double press (2 taps <400ms) | Cycle screens (Usage → Hero → EMO → Splash → Usage) |
| Long press (≥800ms) | Space key down (voice PTT); release = key up |

## Wiring
| LCD Pin | ESP32-C6 GPIO |
|---|---|
| VCC | 3V3 |
| GND | GND |
| DIN (MOSI) | GPIO10 |
| CLK (SCLK) | GPIO23 |
| CS | GPIO5 |
| DC | GPIO4 |
| RST | GPIO6 |
| BL | GPIO7 |

## Rotation
ST7789V2 supports hardware MADCTL rotation. gfx->setRotation(r) switches
display natively. On 90°/270° swap width↔height, LVGL display resolution
is updated via lv_display_set_resolution().
