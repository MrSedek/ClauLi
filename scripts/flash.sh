#!/usr/bin/env bash
# Build + flash the firmware for a given PlatformIO env.
# Usage: scripts/flash.sh <env> [serial-port]
#   env  : esp32c6 | esp32s3 | esp32 | esp32c3
#   port : optional; auto-detected from /dev/cu.usbmodem* if omitted
set -euo pipefail

ENV="${1:-}"
PORT="${2:-}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"

if [[ -z "$ENV" ]]; then
  echo "Usage: $0 <esp32c6|esp32s3|esp32|esp32c3> [serial-port]" >&2
  exit 1
fi

# Locate pio
PIO="$(command -v pio || true)"
[[ -z "$PIO" && -x "$HOME/.platformio/penv/bin/pio" ]] && PIO="$HOME/.platformio/penv/bin/pio"
if [[ -z "$PIO" ]]; then
  echo "PlatformIO 'pio' not found. Install: https://platformio.org/install/cli" >&2
  exit 1
fi

# Auto-detect port (macOS: /dev/cu.usbmodem*, Linux: /dev/ttyACM*/ttyUSB*)
if [[ -z "$PORT" ]]; then
  PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
  [[ -z "$PORT" ]] && PORT="$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1 || true)"
fi
if [[ -z "$PORT" ]]; then
  echo "No serial port found. Pass it explicitly: $0 $ENV /dev/cu.usbmodemXXXX" >&2
  exit 1
fi

echo "Flashing env=$ENV port=$PORT"
exec "$PIO" run -d "$REPO/firmware" -e "$ENV" -t upload --upload-port "$PORT"
