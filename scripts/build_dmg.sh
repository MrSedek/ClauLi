#!/bin/bash
# Build ClauLi-macOS.dmg
#
# Usage:
#   ./scripts/build_dmg.sh [version]
#
# Requires macOS with hdiutil (built-in, no extra tools needed).
# Must be run AFTER `python setup_macos.py py2app` has produced dist/ClauLi.app.
#
# Outputs:
#   daemon/dist/ClauLi-macOS.dmg  — mountable disk image ready for distribution
#
# DMG layout (what the user sees when they open it):
#   ┌──────────────────────────────────────────┐
#   │  ClauLi.app          Applications ->     │
#   │                                          │
#   │  Install ClauLi.command                  │
#   └──────────────────────────────────────────┘
#   • Drag ClauLi.app onto Applications, OR
#   • Double-click "Install ClauLi" for one-click install with admin prompt.
set -euo pipefail

VERSION="${1:-dev}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_SRC="$REPO_ROOT/daemon/dist/ClauLi.app"
INSTALL_SRC="$REPO_ROOT/daemon/installer/install.command"
DMG_OUT="$REPO_ROOT/daemon/dist/ClauLi-macOS.dmg"
VOLUME_NAME="ClauLi $VERSION"

# ── pre-flight checks ─────────────────────────────────────────────────────
if [ ! -d "$APP_SRC" ]; then
    echo "ERROR: $APP_SRC not found." >&2
    echo "Run 'cd daemon && python setup_macos.py py2app' first." >&2
    exit 1
fi
if [ ! -f "$INSTALL_SRC" ]; then
    echo "ERROR: $INSTALL_SRC not found." >&2
    exit 1
fi

# ── staging area ──────────────────────────────────────────────────────────
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

echo "Staging contents..."
cp -R "$APP_SRC" "$STAGE/ClauLi.app"
ln -s /Applications "$STAGE/Applications"
cp "$INSTALL_SRC" "$STAGE/Install ClauLi.command"
chmod +x "$STAGE/Install ClauLi.command"

# ── build read-write image, apply window layout, compress ─────────────────
rm -f "$DMG_OUT"

echo "Building DMG (version $VERSION)..."

# Step 1 — create read-write image so we can set DS_Store / window size.
RW_DMG="$(mktemp).dmg"
hdiutil create \
    -volname "$VOLUME_NAME" \
    -srcfolder "$STAGE" \
    -ov \
    -format UDRW \
    -fs HFS+ \
    "$RW_DMG"

# Step 2 — mount and apply minimal Finder view settings via osascript.
MOUNT_POINT=$(hdiutil attach -readwrite -noverify -noautoopen "$RW_DMG" | awk -F'\t' '/\/Volumes\//{print $NF}' | tail -1)

# Give Finder a moment to notice the new volume.
sleep 1

osascript <<APPLESCRIPT 2>/dev/null || true
tell application "Finder"
    tell disk "$VOLUME_NAME"
        open
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set bounds of container window to {200, 120, 760, 460}
        set the arrangement of icon view options of container window to not arranged
        set the icon size of icon view options of container window to 100
        -- positions: {x, y} from top-left of window
        set position of item "ClauLi.app"              of container window to {140, 160}
        set position of item "Applications"            of container window to {400, 160}
        set position of item "Install ClauLi.command"  of container window to {270, 290}
        close
    end tell
end tell
APPLESCRIPT

# Flush Finder changes before unmounting.
sync
hdiutil detach "$MOUNT_POINT" -quiet || hdiutil detach "$MOUNT_POINT" -quiet -force

# Step 3 — compress to UDZO (zlib).
echo "Compressing..."
hdiutil convert "$RW_DMG" -format UDZO -o "$DMG_OUT" -quiet
rm -f "$RW_DMG"

SIZE=$(du -sh "$DMG_OUT" | cut -f1)
echo "Done: $DMG_OUT ($SIZE)"
