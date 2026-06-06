# ClauLi

**A desk accessory that watches your Claude Code session so you don't have to.**

> [Русская версия](README_RU.md)

ClauLi is an ESP32-based hardware monitor that displays a robot-eye character on a small colour screen. The character's halo colour, eye mood, and on-screen stats update in real time to reflect your Claude Code session usage and weekly token consumption. A macOS/Linux/Windows Python daemon reads your Claude Code OAuth token, polls the Anthropic API, and streams data to the device over Bluetooth Low Energy.

<!-- Add a photo or screenshot here, e.g. ![ClauLi on desk](docs/screenshot.png) -->

---

## Features

- **3 selectable characters** — ClauLi (default), Pixl, and Old-TV, each with a distinct visual style.
- **Live emotions & animations** — 7 eye moods (neutral, curious, sleepy, alarmed, and more) change on a random 20 s–10 min timer; blink animation runs continuously.
- **Usage-to-colour gradient** — eyes and halo smoothly interpolate green → amber → red as weekly token usage climbs from 0 % to 100 %.
- **8 on-screen stat layouts** — `none`, `bezel_orbit`, `twin_columns`, `hud_ribbon`, `tear_pearls`, `corner_chip`, `ecg_monitor`, `classic` — pick the one that fits your desk.
- **Clock** — the daemon sends the current timestamp and timezone; the device advances it locally between polls. Multiple clock styles available.
- **Configurable usage text** — show nothing, percentage, reset-time countdown, or both.
- **BLE link** — pairing-free Bluetooth Low Energy connection; the daemon rescans and reconnects automatically with exponential backoff.
- **Web configurator** — single-page UI at `http://localhost:8765`: character, layout, clock, colour stops, text mode, orientation, per-status forms and animations — all live, no reflashing.
- **RU / EN language** — toggle from the web UI or with a long button press on the device; the choice is persisted in NVS and survives reboots.
- **OTA firmware update** — upload a new `firmware.bin` from the web UI (Settings → Firmware OTA); no USB cable needed after the first flash.

---

## Hardware

| Item | Details |
|---|---|
| MCU board | Any ESP32-C6, ESP32-S3, ESP32-C3, or ESP32 board. Reference: **NanoESP32-C6 v1.0** |
| Display | **ST7789V2** 240×280, SPI |
| Default build env | `esp32c6` (see `firmware/platformio.ini`) |

### Wiring (ESP32-C6, verified reference)

| Signal | GPIO |
|---|---|
| LCD MOSI | 10 |
| LCD SCLK | 23 |
| LCD CS | 5 |
| LCD DC | 4 |
| LCD RESET | 6 |
| LCD Backlight | 7 |
| BOOT button | 9 |
| Button A | 7 |
| Button B | 6 |

Other supported chips (ESP32-S3, ESP32, ESP32-C3) and their sample pin assignments are listed in `firmware/platformio.ini`. Non-C6 values are examples — verify against your actual wiring and edit the `build_flags` for that env.

---

## Quick start

### 1. Build & flash firmware

**Prerequisite:** [PlatformIO CLI](https://platformio.org/install/cli) — `pip install platformio`.

Build for ESP32-C6 (default):

```bash
scripts/build.sh
```

Build and flash over USB in one step (auto-detects the serial port):

```bash
scripts/build.sh --upload
```

Flash only (after a previous build):

```bash
scripts/flash-esp32c6.sh                        # auto-detect port
scripts/flash-esp32c6.sh /dev/cu.usbmodemXXXX   # explicit port
```

Find your serial port:

```bash
ls /dev/cu.usbmodem*           # macOS
ls /dev/ttyACM* /dev/ttyUSB*   # Linux
```

After the first USB flash, future firmware updates can be pushed wirelessly via the web UI (Settings → Firmware OTA).

### 2. Install and run the daemon

#### Option A — Download a release binary (easiest)

Grab the latest release from [GitHub Releases](https://github.com/MrSedek/ClauLi/releases):

| Platform | Artifact | How to run |
|---|---|---|
| macOS | `ClauLi-macOS.zip` | Unzip, drag `ClauLi.app` to `/Applications`, double-click. On first launch: Right-click → Open to bypass Gatekeeper. |
| Linux x86_64 | `clauli-linux-x86_64.tar.gz` | `tar xzf clauli-linux-x86_64.tar.gz && ./clauli/clauli --tray` |
| Windows x86_64 | `ClauLi-windows-x86_64.zip` | Unzip, run `clauli.exe --tray`. Click *More info → Run anyway* on the SmartScreen prompt. |

> **macOS Bluetooth permission — required.** On first launch click **Allow**
> when macOS asks for Bluetooth. If the device is never found and the log
> (`~/.config/claude-usage-monitor/daemon.log`) repeats *"Bluetooth access
> denied"* / *"Scanning…"*, enable **ClauLi** under **System Settings →
> Privacy & Security → Bluetooth**, then fully quit and relaunch (macOS only
> checks at startup). Running from source? Grant Bluetooth to your terminal.

#### Option B — Run from source

Requires Python 3.10+. The script creates a venv and installs all dependencies automatically:

```bash
scripts/run-daemon.sh               # foreground, English (default)
scripts/run-daemon.sh --lang ru     # foreground, Russian
scripts/run-daemon.sh --port 8888   # custom web port
```

Use the lifecycle wrapper for background mode:

```bash
cd daemon
./daemon.sh start                   # background; logs → ~/.config/claude-usage-monitor/daemon.log
./daemon.sh stop
./daemon.sh status
./daemon.sh logs -f
```

#### Option C — macOS launchd agent (auto-start on login)

```bash
scripts/install-daemon-macos.sh              # English, port 8765
scripts/install-daemon-macos.sh --lang ru    # Russian
scripts/install-daemon-macos.sh --port 8888  # custom port
```

This installs a LaunchAgent plist, sets up a venv, and loads it immediately. Logs: `~/Library/Logs/clauli.{out,err}.log`.

### 3. Open the web configurator

Once the daemon is running, open:

```
http://localhost:8765
```

Choose the character, layout, clock style, colour stops, text mode, orientation, and language. All changes are sent to the device live over BLE.

---

## How it works

```
Anthropic API
      |
      | (OAuth, polls every 60 s)
      v
  Python daemon  ──── BLE GATT (pairing-free) ────>  ESP32
  (port 8765)                                          ST7789V2 240x280
      |
      ^
  Web browser  <────  http://localhost:8765
```

1. The daemon polls the Anthropic usage API every 60 seconds (the inner loop ticks every 5 s to catch BLE disconnects quickly).
2. It builds a compact JSON payload — token counts, percentages, timestamp, timezone, and visual config — and writes it to the device's GATT RX characteristic over BLE.
3. The firmware unpacks the payload, updates the character's colour and mood, and redraws the screen.
4. The web configurator (served by the daemon's built-in aiohttp server) lets you tweak every visual parameter without reflashing.

---

## OAuth / re-login

The daemon reads the Claude Code OAuth token from the **macOS Keychain** (service name `Claude Code-credentials`) and writes the required credential files to `~/.config/anthropic/`. On Linux and Windows the token must be set via `claude setup-token` or exported as an environment variable.

**If you see `invalid_grant` / `Refresh token not found or invalid` in the daemon log:**

- **Re-login (recommended):** open the Claude Code app or run `claude` in a terminal and complete the login flow. The daemon force-re-reads the Keychain on every poll and recovers automatically — watch for `AUTH: recovered` in the log.
- **Headless fallback:** run `claude setup-token`, then export the token before starting the daemon:

```bash
export CLAUDE_CODE_OAUTH_TOKEN=<your-token>
scripts/run-daemon.sh
```

- **Verify the Keychain entry (macOS):**

```bash
security find-generic-password -s "Claude Code-credentials" -w \
  | python3 -c "import sys,json; print(json.load(sys.stdin)['claudeAiOauth']['expiresAt'])"
```

The web UI also shows a re-login notice when the token is expired.

---

## Links

- Detailed install guide: [docs/INSTALL.md](docs/INSTALL.md)
- Promo landing page: [docs/site/index.html](docs/site/index.html)
- Releases: [github.com/MrSedek/ClauLi/releases](https://github.com/MrSedek/ClauLi/releases)

---

## Author

Made by **MrSedek**

- GitHub: [github.com/MrSedek/ClauLi](https://github.com/MrSedek/ClauLi)
- Telegram: [t.me/sedek](https://t.me/sedek)
