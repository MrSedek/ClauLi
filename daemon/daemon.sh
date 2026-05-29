#!/usr/bin/env bash
# ClauLi daemon lifecycle wrapper.
#
# Usage:
#   ./daemon.sh start [daemon-args...]   — start daemon in background (logs → file)
#   ./daemon.sh stop                     — stop the running daemon
#   ./daemon.sh status                   — show running status + telemetry
#   ./daemon.sh restart [daemon-args...] — stop then start
#   ./daemon.sh logs [-f]                — tail the daemon.log (pass -f for follow)
#
# Background mode is implemented via `nohup` + PID-file. stdout/stderr go to
# the rotating daemon.log; the foreground `python3 daemon/claude_usage_daemon.py`
# invocation is still available for users who want live console output.
#
# Status command does NOT spawn a daemon — it hits the running one over HTTP.
# If the daemon isn't running, status reports "not running" with exit code 1.

set -e

CONFIG_DIR="$HOME/.config/claude-usage-monitor"
PID_FILE="$CONFIG_DIR/daemon.pid"
LOG_FILE="$CONFIG_DIR/daemon.log"
# Separate sink for the nohup stdout/stderr capture. Python's structured
# `log()` writes directly to LOG_FILE — if we also redirected nohup
# stdout to LOG_FILE, EVERY line would appear TWICE (once via print(),
# once via the file-write path inside log()). STDOUT_FILE holds only
# raw stdout (early-boot diagnostics + crash tracebacks).
STDOUT_FILE="$CONFIG_DIR/daemon.stdout.log"
PYTHON_BIN="${PYTHON:-$(command -v python3)}"
DAEMON_PY="$(cd "$(dirname "$0")" && pwd)/claude_usage_daemon.py"

if [ ! -x "$PYTHON_BIN" ]; then
    echo "daemon: python3 not found (set PYTHON env var to override)" >&2
    exit 1
fi
if [ ! -f "$DAEMON_PY" ]; then
    echo "daemon: $DAEMON_PY missing" >&2
    exit 1
fi

# Returns 0 if running, 1 otherwise. Cleans stale PID-file.
_is_running() {
    [ -f "$PID_FILE" ] || return 1
    local pid
    pid=$(cat "$PID_FILE" 2>/dev/null || true)
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        rm -f "$PID_FILE"
        return 1
    fi
    return 0
}

cmd_start() {
    mkdir -p "$CONFIG_DIR"
    if _is_running; then
        echo "daemon: already running (PID $(cat "$PID_FILE"))"
        exit 1
    fi
    # nohup + & — fully detached background. stdout/stderr go to a
    # DIFFERENT file (daemon.stdout.log) than the structured log to
    # prevent the duplicate-line bug: Python's log() already appends to
    # daemon.log, so if nohup also redirects stdout into daemon.log we
    # get every line written twice.
    nohup "$PYTHON_BIN" "$DAEMON_PY" "$@" >>"$STDOUT_FILE" 2>&1 &
    local pid=$!
    echo "$pid" > "$PID_FILE"
    # Brief sleep + recheck: if the daemon exits immediately (port conflict,
    # bad arg, …) we want to report failure rather than claim success.
    sleep 1
    if kill -0 "$pid" 2>/dev/null; then
        echo "daemon: started (PID $pid)"
        echo "  structured log: $LOG_FILE"
        echo "  stdout/stderr : $STDOUT_FILE"
    else
        rm -f "$PID_FILE"
        echo "daemon: failed to start — last 20 stdout lines:" >&2
        tail -n 20 "$STDOUT_FILE" 2>/dev/null >&2 || true
        echo "--- structured log:" >&2
        tail -n 20 "$LOG_FILE" 2>/dev/null >&2 || true
        exit 1
    fi
}

cmd_stop() {
    if ! _is_running; then
        echo "daemon: not running"
        exit 1
    fi
    local pid
    pid=$(cat "$PID_FILE")
    kill "$pid" 2>/dev/null || true
    # Wait up to 5 s for graceful exit; SIGKILL if still alive.
    local i=0
    while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 50 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        echo "daemon: graceful stop timed out — sending SIGKILL"
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$PID_FILE"
    echo "daemon: stopped (PID $pid)"
}

cmd_status() {
    if _is_running; then
        echo "daemon: running (PID $(cat "$PID_FILE"))"
        # Reach into the live daemon over HTTP — gives BLE + last-data view
        # in addition to PID. The Python --status path now has graceful
        # fallback so any failure here is a soft warning, not a crash.
        "$PYTHON_BIN" "$DAEMON_PY" --status 2>/dev/null || true
    else
        echo "daemon: not running"
        exit 1
    fi
}

cmd_logs() {
    if [ ! -f "$LOG_FILE" ]; then
        echo "daemon: log file does not exist yet ($LOG_FILE)"
        exit 1
    fi
    if [ "$1" = "-f" ] || [ "$1" = "--follow" ]; then
        tail -n 200 -f "$LOG_FILE"
    else
        tail -n 200 "$LOG_FILE"
    fi
}

case "${1:-}" in
    start)   shift; cmd_start "$@" ;;
    stop)    cmd_stop ;;
    status)  cmd_status ;;
    restart) shift; cmd_stop 2>/dev/null || true; cmd_start "$@" ;;
    logs)    shift; cmd_logs "$@" ;;
    *)
        cat <<EOF >&2
Usage: $0 {start|stop|status|restart|logs} [args]

  start [daemon-args...]   Start daemon in background, redirect logs to $LOG_FILE.
                           Pass-through args to python: --lang ru, --port 8888 etc.
  stop                     Stop the running daemon (SIGTERM → SIGKILL after 5s).
  status                   Show running status + BLE / last-data summary.
                           Exits 1 if daemon not running.
  restart [daemon-args...] Stop then start.
  logs [-f|--follow]       Tail $LOG_FILE (200 lines; -f for live follow).

For foreground console mode (current behaviour, useful for debugging) use:
  python3 $DAEMON_PY [daemon-args...]
EOF
        exit 2
        ;;
esac
