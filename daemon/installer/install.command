#!/bin/bash
# ClauLi Installer
# Double-click this file from the DMG to install ClauLi to /Applications.
# The script copies ClauLi.app, removes the quarantine flag, and launches
# the app so macOS can prompt for Bluetooth permission right away.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_SRC="$SCRIPT_DIR/ClauLi.app"
APP_DEST="/Applications/ClauLi.app"

# ── sanity: app must be next to this script (inside the DMG) ──────────────
if [ ! -d "$APP_SRC" ]; then
    osascript -e 'display alert "ClauLi.app not found." message "Make sure Install ClauLi.command is in the same folder as ClauLi.app (inside the DMG)." as critical buttons {"OK"}' 2>/dev/null || true
    exit 1
fi

# ── confirm ───────────────────────────────────────────────────────────────
MSG="Install ClauLi to /Applications?\n\nYou will be asked for your administrator password."
CONFIRM=$(osascript -e "display dialog \"$MSG\" with title \"Install ClauLi\" buttons {\"Cancel\", \"Install\"} default button \"Install\" with icon note" 2>/dev/null || echo "cancelled")
[[ "$CONFIRM" != *"Install"* ]] && exit 0

# ── copy + remove quarantine (needs admin) ────────────────────────────────
# Use escaped single quotes so paths with spaces are handled correctly.
ESCAPED_SRC="${APP_SRC//\'/\'\\\'\'}"
SHELL_CMD="cp -Rf '$ESCAPED_SRC' /Applications/ && xattr -dr com.apple.quarantine '$APP_DEST' 2>/dev/null; true"

if ! osascript -e "do shell script \"$SHELL_CMD\" with administrator privileges" 2>/dev/null; then
    osascript -e 'display alert "Installation failed." message "Could not copy ClauLi to /Applications.\nCheck that you have administrator access and try again." as critical buttons {"OK"}' 2>/dev/null || true
    exit 1
fi

# ── launch (triggers Bluetooth permission dialog on first run) ────────────
open "$APP_DEST" 2>/dev/null || true

osascript -e 'display dialog "ClauLi installed successfully!\n\nThe ClauLi icon will appear in your menu bar.\n\nOn first launch macOS will ask for Bluetooth access — click Allow so ClauLi can reach your ESP32 device." with title "ClauLi installed" buttons {"OK"} default button "OK" with icon note' 2>/dev/null || true
