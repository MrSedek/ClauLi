#!/bin/bash
# ClauLi debug log capture — collects ESP serial + daemon log simultaneously.
# Usage:
#   ./capture_logs.sh               # saves to /tmp/clauli_YYYYMMDD_HHMMSS/
#   ./capture_logs.sh /my/dir       # saves to that directory
#   ./capture_logs.sh - | tee file  # stream both logs merged to stdout
#
# Output files (in the log directory):
#   esp.log     — raw ESP32 serial output, one line per message with timestamp
#   daemon.log  — tailed from the daemon's log file (or stdout hint if not found)
#   merged.log  — both streams interleaved with source tags [ESP] / [DAEMON]
#
# After capturing, share the merged.log (or both separate logs) for diagnosis.

set -euo pipefail

PORT="${CLAULI_PORT:-/dev/cu.usbmodem101}"
BAUD=115200
DAEMON_LOG="${CLAULI_DAEMON_LOG:-}"  # optional: path to daemon log file

# Resolve output directory
if [ "${1:-}" = "-" ]; then
  OUTDIR=""
else
  TS=$(date +%Y%m%d_%H%M%S)
  OUTDIR="${1:-/tmp/clauli_$TS}"
  mkdir -p "$OUTDIR"
  echo "ClauLi log capture → $OUTDIR"
  echo "  ESP port:  $PORT"
  echo "  Press Ctrl-C to stop."
  echo ""
fi

# ─── ESP serial capture ──────────────────────────────────────────────────────
capture_esp() {
  local outfile="$1"
  /Users/sedek/.platformio/penv/bin/python3 - "$PORT" "$outfile" <<'PY'
import serial, sys, time, signal, datetime

port_path = sys.argv[1]
outfile   = sys.argv[2] if len(sys.argv) > 2 else "-"

def ts():
    return datetime.datetime.now().strftime("[%H:%M:%S.%f")[:-3] + "]"

try:
    s = serial.Serial(port_path, 115200, timeout=0.5)
except Exception as e:
    print(f"ESP serial OPEN FAILED: {e}", file=sys.stderr)
    sys.exit(1)

out = open(outfile, "w", buffering=1) if outfile != "-" else sys.stdout
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

try:
    while True:
        ln = s.readline()
        if not ln:
            continue
        try:
            txt = ln.decode("utf-8", errors="replace").rstrip()
        except Exception:
            continue
        line = f"{ts()} {txt}"
        out.write(line + "\n")
        if outfile != "-":
            # Mirror key BLE/boot lines to stdout with [ESP] tag for merged.log
            if any(k in txt for k in ("BLE:", "[BLE]", "[BOOT]", "[HEAP]", "[CFG]",
                                       "disconnect", "connected", "advertising",
                                       "reason=", "panic", "abort")):
                print(f"[ESP]    {line}", flush=True)
except (KeyboardInterrupt, SystemExit):
    pass
finally:
    s.close()
    if outfile != "-":
        out.close()
PY
}

# ─── Daemon log capture ───────────────────────────────────────────────────────
# Daemon writes to its terminal. If user has redirected it to a file, tail it.
# Otherwise print a hint.
capture_daemon() {
  local outfile="$1"
  if [ -n "$DAEMON_LOG" ] && [ -f "$DAEMON_LOG" ]; then
    tail -F "$DAEMON_LOG" 2>/dev/null | while IFS= read -r line; do
      echo "$line" >> "$outfile"
      echo "[DAEMON] $line"
    done
  else
    echo "[DAEMON] Daemon log file not found."                             >&2
    echo "[DAEMON] To capture daemon logs, redirect them to a file:"      >&2
    echo "[DAEMON]   ./daemon/claude_usage_daemon.py 2>&1 | tee /tmp/clauli_daemon.log"  >&2
    echo "[DAEMON] Then re-run with: CLAULI_DAEMON_LOG=/tmp/clauli_daemon.log ./capture_logs.sh" >&2
    # Keep this process alive so the ESP side can still capture
    sleep infinity
  fi
}

if [ -z "$OUTDIR" ]; then
  # Stream mode: just capture ESP to stdout
  capture_esp "-"
else
  ESP_LOG="$OUTDIR/esp.log"
  DAEMON_LOG_OUT="$OUTDIR/daemon.log"
  MERGED_LOG="$OUTDIR/merged.log"

  # Run both captures, merging stdout to merged.log
  {
    capture_esp "$ESP_LOG" &
    ESP_PID=$!
    capture_daemon "$DAEMON_LOG_OUT" &
    DAEMON_PID=$!
    wait $ESP_PID $DAEMON_PID
  } | tee "$MERGED_LOG"

  echo ""
  echo "Logs saved:"
  echo "  $ESP_LOG"
  echo "  $DAEMON_LOG_OUT"
  echo "  $MERGED_LOG  ← share this for diagnosis"
fi
