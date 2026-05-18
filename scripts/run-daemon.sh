#!/usr/bin/env bash
# Bootstrap a venv, install deps, and run the daemon in the foreground.
# Usage: scripts/run-daemon.sh [--lang en|ru] [--port N]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
VENV="$REPO/.venv"

if [[ ! -d "$VENV" ]]; then
  echo "Creating venv at $VENV"
  python3 -m venv "$VENV"
fi
# shellcheck disable=SC1091
source "$VENV/bin/activate"
pip install -q --upgrade pip
pip install -q -r "$REPO/daemon/requirements.txt"

exec python "$REPO/daemon/claude_usage_daemon.py" "$@"
