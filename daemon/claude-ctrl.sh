#!/usr/bin/env bash
# claude-ctrl — CLI wrapper for ClauLi daemon control
set -euo pipefail

DAEMON_SCRIPT="$(cd "$(dirname "$0")" && pwd)/claude_usage_daemon.py"
PORT="${CLAULI_PORT:-8765}"

usage() {
  cat <<EOF
Usage: claude-ctrl <command>

Commands:
  usage      Switch ESP32 to Usage screen
  emo        Switch ESP32 to EMO screen
  bt         Switch ESP32 to Bluetooth screen
  splash     Switch ESP32 to Splash screen
  cycle      Cycle ESP32 screens
  refresh    Force data refresh on ESP32
  status     Show current daemon status
  ui         Open Web UI in browser
  help       Show this help

Environment:
  CLAULI_PORT  HTTP port (default: 8765)
EOF
}

case "${1:-help}" in
  usage|emo|bt|splash|cycle)
    python3 "$DAEMON_SCRIPT" --screen "$1" --port "$PORT"
    ;;
  refresh)
    python3 "$DAEMON_SCRIPT" --refresh --port "$PORT"
    ;;
  status)
    python3 "$DAEMON_SCRIPT" --status --port "$PORT"
    ;;
  ui)
    open "http://localhost:$PORT"
    ;;
  help|--help|-h)
    usage
    ;;
  *)
    echo "Unknown command: $1" >&2
    usage >&2
    exit 1
    ;;
esac
