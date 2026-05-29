# PyInstaller spec for ClauLi — Linux & Windows
#
# Usage:
#     cd daemon
#     pip install pyinstaller pillow pystray
#     pyinstaller --clean --noconfirm clauli.spec
#
# Produces dist/clauli/ (onedir) — preferred over --onefile because aiohttp
# + bleak start ~3× faster from an unpacked tree, and the web/ assets stay
# trivially editable for power users.

# -*- mode: python ; coding: utf-8 -*-
import os
import sys
from pathlib import Path

block_cipher = None

# Resolve daemon dir without assuming CWD
HERE = Path(os.getcwd()).resolve()
if not (HERE / "claude_usage_daemon.py").exists():
    # Fall back to the spec file's location
    HERE = Path(__file__).resolve().parent

# Single entry — same module the macOS bundle uses, except we don't force
# --tray (Linux/Windows users may want CLI mode too; the bundled binary
# accepts the same argv as the python script).
entry_script = str(HERE / "claude_usage_daemon.py")

# Data files: bundle the entire web/ tree under daemon/web/ so the aiohttp
# server can serve /index.html from the frozen build.
datas = [
    (str(HERE / "web"), "web"),
]

# Hidden imports — modules that get imported indirectly (e.g. via getattr
# or string lookup) and PyInstaller's static analysis misses.
hiddenimports = [
    "tray", "autostart",
    # bleak's backend is picked at runtime; force include the right one(s)
    "bleak.backends.bluezdbus.client",          # Linux
    "bleak.backends.bluezdbus.scanner",
    "bleak.backends.winrt.client",              # Windows
    "bleak.backends.winrt.scanner",
    # pystray's backend likewise
    "pystray._gtk", "pystray._xorg",
    "pystray._win32",
    "PIL._tkinter_finder",                       # PIL detection helper
]

# Drop unused stdlib bloat
excludes = [
    "tkinter", "test", "unittest", "pydoc", "doctest",
    # We never use these; PyInstaller defaults pull them in
    "tcl", "tk",
]

a = Analysis(
    [entry_script],
    pathex=[str(HERE)],
    binaries=[],
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    runtime_hooks=[],
    excludes=excludes,
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)
pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

# Console flag: True on Linux (visible stdout for debugging), False on
# Windows (no flashing cmd.exe window when launched from autostart).
console_flag = sys.platform != "win32"

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="clauli",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=console_flag,
    icon=None,            # drop a .ico/.icns next to this file when ready
    disable_windowed_traceback=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)

coll = COLLECT(
    exe,
    a.binaries,
    a.zipfiles,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name="clauli",
)
