"""py2app build script for ClauLi.app (macOS bundle).

Usage:
    cd daemon
    pip install py2app pillow rumps
    python setup_macos.py py2app          # full bundle in dist/ClauLi.app
    python setup_macos.py py2app -A       # alias mode — fast iteration

The bundle launches `claude_usage_daemon.py --tray`. CLI-mode (foreground
daemon) is still available via daemon.sh — the bundled app is only the
GUI surface.
"""
from setuptools import setup

# ── Bundle metadata ─────────────────────────────────────────────────────────
APP_NAME = "ClauLi"
VERSION = "0.1.0"
BUNDLE_ID = "com.sedek.clauli"

# ── App entry: tiny shim that imports the daemon and forces --tray ──────────
# We point py2app at a thin wrapper rather than claude_usage_daemon.py directly
# so the bundle's argv defaults to tray mode without the user passing flags.
APP = ["macos_entry.py"]

# ── Resources we need inside the bundle ─────────────────────────────────────
# The web UI is served by aiohttp out of <bundle>/Resources/web/, so we have
# to copy the whole web/ tree alongside the .py code. py2app's `resources`
# option does this — single-element list of "include this directory verbatim".
DATA_FILES = ["web"]

OPTIONS = {
    "argv_emulation": False,
    "iconfile": None,  # optional: drop a .icns next to this file later
    "plist": {
        "CFBundleName":            APP_NAME,
        "CFBundleDisplayName":     APP_NAME,
        "CFBundleIdentifier":      BUNDLE_ID,
        "CFBundleVersion":         VERSION,
        "CFBundleShortVersionString": VERSION,
        "LSUIElement":             True,   # ← menu-bar-only, no Dock icon
        "NSHighResolutionCapable": True,
        # Required for bleak / CoreBluetooth — without this the OS denies BLE
        # access silently and the daemon shows "scanning forever".
        "NSBluetoothAlwaysUsageDescription":
            "ClauLi talks to your ESP32 desk monitor over Bluetooth Low Energy.",
        "NSBluetoothPeripheralUsageDescription":
            "ClauLi talks to your ESP32 desk monitor over Bluetooth Low Energy.",
    },
    "packages": [
        "aiohttp", "bleak", "httpx", "anthropic", "rumps",
    ],
    "includes": [
        # Stdlib modules py2app's static analysis sometimes misses
        "asyncio", "ssl", "json", "logging", "signal", "threading",
        # Our own modules
        "tray", "autostart",
    ],
    "excludes": [
        # py2app pulls in tkinter unless told otherwise → ~20 MB bloat
        "tkinter", "test", "unittest", "pydoc",
    ],
    "site_packages": True,
    "strip": True,
}

setup(
    app=APP,
    name=APP_NAME,
    version=VERSION,
    data_files=DATA_FILES,
    options={"py2app": OPTIONS},
    setup_requires=["py2app"],
)
