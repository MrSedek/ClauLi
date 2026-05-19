# ClauLi

A small ESP32 desk dashboard for Claude Code usage. The firmware drives a
**ST7789V2 240×280 SPI LCD** and advertises a BLE service; a Python daemon on
your computer polls the Anthropic API for rate-limit usage and pushes it to the
device. Screens: animated splash, usage gauges, an animated **EMO** eyes
screen, and a Bluetooth status screen.

UI language is **English by default**. Switch RU/EN on the device with a
**long-press of the BOOT button** (persisted in NVS). The daemon's
`--lang ru` flag only seeds the language until you set it on-device.

Build by Sedek · https://github.com/MrSedek · https://t.me/sedek

---

## Architecture

```
Anthropic API ──poll──> Python daemon ──BLE GATT──> ESP32 firmware ──> LCD
                          (host computer)            (any supported board)
```

- Firmware: NimBLE peripheral, LVGL 9 UI, Arduino_GFX ST7789 driver.
- Daemon: `bleak` BLE client + `anthropic` SDK (OAuth via macOS Keychain) +
  an `aiohttp` web UI/REST server on `:8765`.
- The daemon sends only numbers; all UI text lives in the firmware (i18n).

---

## Supported boards

| Env (`-e`) | Chip | Board ID | Pin status |
|------------|------|----------|-----------|
| `esp32c6`  | ESP32-C6 | `sparkfun_esp32c6_thing_plus` | **Verified** (reference) |
| `esp32s3`  | ESP32-S3 | `esp32-s3-devkitc-1` | Sample — verify wiring |
| `esp32`    | ESP32 | `esp32dev` | Sample — verify wiring |
| `esp32c3`  | ESP32-C3 | `esp32-c3-devkitm-1` | Sample — verify wiring |

Pins are set per-env in `firmware/platformio.ini` via `-DPIN_LCD_*` /
`-DPIN_BTN_*` build flags (`firmware/src/display_cfg.h` holds the C6
fallbacks). **Only the ESP32-C6 wiring is verified.** For other chips the
values are reasonable samples — confirm against your board and edit the
`build_flags` for that env.

### Wiring (display = ST7789V2 240×280, SPI)

| Signal | Flag | C6 | S3 (sample) | ESP32 (sample) | C3 (sample) |
|--------|------|---:|------------:|---------------:|------------:|
| MOSI/DIN | `PIN_LCD_MOSI` | 10 | 11 | 23 | 6 |
| SCLK/CLK | `PIN_LCD_SCLK` | 23 | 12 | 18 | 4 |
| CS       | `PIN_LCD_CS`   | 5  | 10 | 5  | 7 |
| DC       | `PIN_LCD_DC`   | 4  | 13 | 2  | 5 |
| RESET    | `PIN_LCD_RESET`| 6  | 14 | 4  | 8 |
| Backlight| `PIN_LCD_BL`   | 7  | 21 | 32 | 3 |
| Boot btn | `PIN_BTN_BOOT` | 9  | 0  | 0  | 9 |
| Btn A    | `PIN_BTN_A`    | 7  | 4  | 34 | 1 |
| Btn B    | `PIN_BTN_B`    | 6  | 5  | 35 | 0 |

Display VCC = 3V3, GND = GND. The panel is 240×280 inside a 240×320
controller (row offset 20, handled in firmware).

---

## Firmware: build & flash

Requires [PlatformIO CLI](https://platformio.org/install/cli) (`pio`).

```bash
# Build only
pio run -d firmware -e esp32c6

# Build + flash (auto-detects the serial port)
scripts/flash-esp32c6.sh
# or pass a port explicitly:
scripts/flash-esp32c6.sh /dev/cu.usbmodemXXXX
```

Per-board helpers: `scripts/flash-esp32c6.sh`, `flash-esp32s3.sh`,
`flash-esp32.sh`, `flash-esp32c3.sh` (all wrap `scripts/flash.sh <env> [port]`).

Find the serial port:

```bash
ls /dev/cu.usbmodem*            # macOS
ls /dev/ttyACM* /dev/ttyUSB*    # Linux
```

If a flash drops mid-write ("Lost connection"), just re-run — it's transient
USB, not a build problem.

---

## Daemon: setup & run

The daemon needs Python 3.10+ and reads the Claude Code OAuth token from the
macOS Keychain (service `Claude Code-credentials`). Log in once with Claude
Code so the credential exists.

### Quick run (foreground)

```bash
scripts/run-daemon.sh                 # English UI (default)
scripts/run-daemon.sh --lang ru       # Russian UI
scripts/run-daemon.sh --port 9000     # custom web port
```

`run-daemon.sh` creates `.venv`, installs `daemon/requirements.txt`, and runs
the daemon in the foreground.

### Autostart (macOS launchd)

```bash
scripts/install-daemon-macos.sh                  # en, port 8765
scripts/install-daemon-macos.sh --lang ru --port 8765
```

Sets up the venv, fills `daemon/com.user.claude-usage-daemon.plist`, and loads
it as a LaunchAgent (auto-restart, runs at login). Logs:
`~/Library/Logs/clauli.{out,err}.log`. Unload with
`launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`.

### CLI control (talks to the running daemon)

```bash
daemon/claude-ctrl.sh status     # daemon/BLE status
daemon/claude-ctrl.sh usage      # switch device to Usage screen
daemon/claude-ctrl.sh emo        # EMO screen
daemon/claude-ctrl.sh splash     # splash screen
daemon/claude-ctrl.sh cycle      # cycle screens
daemon/claude-ctrl.sh refresh    # force a data poll
daemon/claude-ctrl.sh ui         # open the web UI
```

Or directly: `python daemon/claude_usage_daemon.py --screen emo --port 8765`.

### Web UI

Open `http://localhost:8765` (or `claude-ctrl.sh ui`): live gauges, BLE
status, screen-switch buttons, manual refresh, and an **EN/RU language
toggle** that also localizes the web page itself (EN/RU). The choice is
saved on the host (`~/.config/claude-usage-monitor/lang`, survives daemon
restarts) and pushed to the device, which also stores it in NVS. Default is
English.

---

## BLE connection

The firmware advertises as **`ClauLi`**. The daemon scans for that name on
every (re)connect and connects to the freshly-discovered peripheral — no
address cache (required on macOS, and it removes the old "connect on the
2nd start" problem). The on-device **Bluetooth** screen has a "Reset
Bluetooth" tap zone to clear bonds.

## Buttons

- **Boot button** 
  - short press: cycle the current screen's content (EMO
  view modes); 
  - double press: cycle screens (Usage → EMO → Splash); 
  - long press: toggle UI language RU↔EN.

Both the **UI language** (default English) and the **EMO view mode** are
saved in NVS and restored on the next boot.

## EMO screen

The device **boots into the EMO screen** by default. Robot-eye emotions
auto-change on a random 20 s–10 min timer. Eye color is a
**smooth gradient** that reddens continuously with usage (green→amber→red,
0→100%). The Boot short-press cycles **10 view modes**: eyes / eyes+text /
eyes+gradient / eyes+text+gradient / compact (one-line on the bars) — then
the same five again, each with a **live clock** at the top. The clock is fed
and re-synced by the daemon (no on-board RTC) and ticks locally between
polls; it shows `--:--` until the daemon connects.

## Color self-test build

`pio run -d firmware -e esp32c6_test -t upload` flashes a build that drives
the compact+clock mode with a synthetic 0→100% sweep — use it to eyeball the
smooth color transition. It does not poll the daemon.

---

## Troubleshooting

- **`Token acquisition failed … HTTP 400`** — the daemon writes the correct
  Claude Code OAuth client to `~/.config/anthropic/configs/default.json`.
  Restart the daemon after updating; re-login with Claude Code if the
  Keychain token is missing.
- **`invalid_grant: Refresh token not found or invalid`** — the refresh
  token was revoked/rotated; it can't refresh silently. Re-authenticate
  in the Claude Code app (or `claude` in a terminal) — the daemon
  re-reads the Keychain every poll and recovers **without a restart**
  (`AUTH: recovered` in the log). See `CLAUDE.md` → "OAuth token /
  re-issue" for the headless `CLAUDE_CODE_OAUTH_TOKEN` fallback.
- **Daemon can't find ClauLi but macOS shows it "Connected"** — macOS
  paired it as an HID keyboard and holds the link, so the ESP stops
  advertising for the daemon. Fix: Forget ClauLi in System Settings →
  Bluetooth and flash a build with `BLE_HID_ENABLED 0` in
  `firmware/src/ble.cpp` (default; daemon-only, no bonding).
- **Tofu boxes in text** — fonts must include the glyph; regenerate per the
  snippet in `CLAUDE.md` (it includes `-r 0x00B7` for the `·` separator).
- **No `screenshot` command** — the firmware has no framebuffer dump; verify
  UI changes on the physical device.
- **Black screen** — wrong display pins for your board; fix that env's
  `-DPIN_LCD_*` flags in `firmware/platformio.ini`.

## Repository layout

```
firmware/   PlatformIO project (src/, platformio.ini)
daemon/     Python daemon, web UI, launchd plist, claude-ctrl.sh
scripts/    flash-*.sh, run-daemon.sh, install-daemon-macos.sh
tools/      font + sprite generators (lv_font_conv patcher, etc.)
```
