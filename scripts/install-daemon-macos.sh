#!/usr/bin/env bash
# Install the ClauLi daemon as a macOS launchd LaunchAgent.
# Sets up a venv, fills the plist template, and loads the agent.
# Usage: scripts/install-daemon-macos.sh [--lang en|ru] [--port N]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
VENV="$REPO/.venv"
LABEL="com.user.claude-usage-daemon"
TEMPLATE="$REPO/daemon/com.user.claude-usage-daemon.plist"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG_DIR="$HOME/Library/Logs"

LANG_ARG="en"
PORT_ARG="8765"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --lang) LANG_ARG="$2"; shift 2 ;;
    --port) PORT_ARG="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

# venv + deps
[[ -d "$VENV" ]] || python3 -m venv "$VENV"
# shellcheck disable=SC1091
source "$VENV/bin/activate"
pip install -q --upgrade pip
pip install -q -r "$REPO/daemon/requirements.txt"
PYTHON_BIN="$VENV/bin/python"

mkdir -p "$HOME/Library/LaunchAgents" "$LOG_DIR"

# Fill the template, then rewrite ProgramArguments to include --lang/--port.
python3 - "$TEMPLATE" "$PLIST" \
  "$PYTHON_BIN" "$REPO/daemon/claude_usage_daemon.py" "$REPO" \
  "$LOG_DIR/clauli.out.log" "$LOG_DIR/clauli.err.log" "$HOME" \
  "$LANG_ARG" "$PORT_ARG" <<'PY'
import sys
tmpl, out, py, daemon, repo, logo, loge, home, lang, port = sys.argv[1:11]
s = open(tmpl).read()
for k, v in {
    "__PYTHON_BIN__": py, "__DAEMON_PATH__": daemon, "__REPO_DIR__": repo,
    "__LOG_OUT__": logo, "__LOG_ERR__": loge, "__HOME__": home,
}.items():
    s = s.replace(k, v)
old = ("        <string>--port</string>\n"
       "        <string>8765</string>\n")
new = (f"        <string>--lang</string>\n        <string>{lang}</string>\n"
       f"        <string>--port</string>\n        <string>{port}</string>\n")
s = s.replace(old, new)
open(out, "w").write(s)
print("wrote", out)
PY

launchctl unload "$PLIST" 2>/dev/null || true
launchctl load "$PLIST"
echo "Loaded $LABEL (lang=$LANG_ARG port=$PORT_ARG)"
echo "Logs: $LOG_DIR/clauli.{out,err}.log"
