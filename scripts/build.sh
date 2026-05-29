#!/usr/bin/env bash
# Quick firmware build + path-to-artifact for OTA upload.
#
# Usage:
#   scripts/build.sh              # build esp32c6 (incremental)
#   scripts/build.sh --upload     # build + flash over USB (whatever /dev/cu.usbmodem* is)
#   scripts/build.sh --env esp32  # pick a different env
#   scripts/build.sh --regen-eyes # also regenerate emo_eyes_hd.h before building
#   scripts/build.sh --force      # `pio run -t clean` first so EVERY .o + .bin
#                                  is regenerated. Use when you suspect the
#                                  incremental build is stale.
#
# After a build the .bin path + mtime is printed. If the mtime is OLDER than
# the build started, that's an "Up to date" no-op build (source unchanged
# since last build) — perfectly fine, but tells you that flashing this bin
# would change nothing.

set -euo pipefail
cd "$(dirname "$0")/.."

ENV=esp32c6
UPLOAD=0
REGEN_EYES=0
FORCE=0
PIO="${HOME}/.platformio/penv/bin/pio"
[[ -x "$PIO" ]] || PIO=pio

while [[ $# -gt 0 ]]; do
    case "$1" in
        --upload)     UPLOAD=1; shift ;;
        --env)        ENV="$2"; shift 2 ;;
        --regen-eyes) REGEN_EYES=1; shift ;;
        --force)      FORCE=1; shift ;;
        -h|--help)
            sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

if [[ "$REGEN_EYES" -eq 1 ]]; then
    echo "→ regenerating firmware/src/emo_eyes_hd.h"
    python3 tools/build_emo_hd.py
fi

if [[ "$FORCE" -eq 1 ]]; then
    echo "→ cleaning build cache (--force)"
    "$PIO" run -d firmware -e "$ENV" -t clean >/dev/null
fi

BIN="firmware/.pio/build/${ENV}/firmware.bin"

# Snapshot mtime BEFORE build so we can detect no-op (incremental) builds.
BEFORE_MTIME=0
if [[ -f "$BIN" ]]; then
    BEFORE_MTIME=$(stat -f%m "$BIN" 2>/dev/null || stat -c%Y "$BIN" 2>/dev/null || echo 0)
fi

echo "→ building firmware (env=$ENV)"
"$PIO" run -d firmware -e "$ENV"

if [[ ! -f "$BIN" ]]; then
    echo "✗ build succeeded but $BIN missing — partition layout or env name changed?" >&2
    exit 1
fi

SIZE=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN" 2>/dev/null)
SIZE_KB=$((SIZE / 1024))
AFTER_MTIME=$(stat -f%m "$BIN" 2>/dev/null || stat -c%Y "$BIN" 2>/dev/null || echo 0)
HUMAN_MTIME=$(stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S' "$BIN" 2>/dev/null \
              || stat -c '%y' "$BIN" 2>/dev/null | cut -d. -f1)

echo
if [[ "$AFTER_MTIME" -gt "$BEFORE_MTIME" ]]; then
    echo "🔨 Recompiled — ${BIN} (${SIZE_KB} KB, ${HUMAN_MTIME})"
else
    echo "↻ Up to date — no rebuild needed (sources unchanged)"
    echo "  ${BIN} (${SIZE_KB} KB, ${HUMAN_MTIME})"
    echo "  ↪ if you edited something but see this message, the change may not"
    echo "    have been saved; re-run with --force to bypass incremental cache"
fi
echo

if [[ "$UPLOAD" -eq 1 ]]; then
    PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1 || true)
    if [[ -z "$PORT" ]]; then
        echo "✗ no /dev/cu.usbmodem* device found — plug in via USB" >&2
        exit 1
    fi
    echo "→ flashing via $PORT"
    "$PIO" run -d firmware -e "$ENV" -t upload --upload-port "$PORT"
else
    echo "Next steps:"
    echo "  • OTA: open the web UI → Settings → Firmware OTA → pick ${BIN}"
    echo "  • USB: scripts/build.sh --upload"
fi
