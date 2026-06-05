#!/usr/bin/env bash
# ── ClauLi USB flasher (macOS / Linux) ───────────────────────────────────
# Flashes the complete firmware image to an ESP32-C6 over USB.
# No build tools required — only Python 3 (used to run esptool, which this
# script installs for you).
#
#   ./flash.sh                 # auto-detect the serial port
#   ./flash.sh /dev/ttyACM0    # or pass it explicitly
#
# After flashing, the eyes should appear on the display within a few seconds.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
IMG="$DIR/firmware.factory.bin"
PORT="${1:-}"

if [[ ! -f "$IMG" ]]; then
  echo "error: firmware.factory.bin not found next to this script." >&2
  exit 1
fi

PY="$(command -v python3 || command -v python || true)"
if [[ -z "$PY" ]]; then
  echo "error: Python 3 not found. Install it from https://python.org and re-run." >&2
  exit 1
fi

echo "Installing esptool (one-time)…"
"$PY" -m pip install --quiet --upgrade esptool

ARGS=(--chip esp32c6)
[[ -n "$PORT" ]] && ARGS+=(--port "$PORT")

echo "Flashing firmware.factory.bin at offset 0x0…"
"$PY" -m esptool "${ARGS[@]}" write_flash 0x0 "$IMG"

echo "Done. If the screen stays blank, unplug and replug the board."
