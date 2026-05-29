#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "ClauLi" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS) and the anthropic SDK for
automatic OAuth token refresh.

Also runs an aiohttp HTTP server with REST API, WebSocket, and Web UI
for remote control of the ESP32 device.
"""

import argparse
import asyncio
import json
import os
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import httpx
from anthropic import Anthropic
from anthropic.lib.credentials import CredentialsFile
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

try:
    from aiohttp import web
except ImportError:
    print("ERROR: aiohttp not installed. Run: pip install aiohttp", file=sys.stderr)
    sys.exit(1)

DEVICE_NAME = "ClauLi"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
TX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000003"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"
CTRL_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"
# OTA service (firmware update over BLE)
OTA_SERVICE_UUID   = "4c41555a-4465-7669-6365-000000000008"
OTA_CTRL_CHAR_UUID = "4c41555a-4465-7669-6365-000000000009"
OTA_DATA_CHAR_UUID = "4c41555a-4465-7669-6365-00000000000a"
OTA_CHUNK_SIZE     = 480     # safe under MTU 517 − ATT 3-byte header

# RX-payload chunking (cfg JSON pushes that exceed the negotiated MTU).
# macOS CoreBluetooth defaults to ATT_MTU = 185 → max single write-without-
# response payload = 182 bytes. We pick 180 to stay safely below, accounting
# for the 1- or 3-byte chunk header on top of the JSON slice. The full emo2
# cfg blob is ~720 B; without chunking it gets silently dropped, which is
# why colour_stops never reached the firmware before this protocol existed.
RX_CHUNK_BUDGET    = 180

# CTRL command bytes (must match firmware ble.cpp)
CTRL_SCREEN_USAGE = 0x01
CTRL_SCREEN_BT = 0x02
CTRL_SCREEN_SPLASH = 0x03
CTRL_SCREEN_CYCLE = 0x04
CTRL_SCREEN_EMO = 0x06
CTRL_SCREEN_EMO2 = 0x09    # ClauLi (HD emo screen with bloom + specular)
CTRL_REBOOT = 0x05         # reboot the ESP32
CTRL_CYCLE_VIEW = 0x07     # BOOT-click analog (cycle the current screen's view)
CTRL_NEXT_ANIM = 0x08      # advance emotion/animation now (skip the timer)
CTRL_DIAGNOSE = 0x0A       # emo2: 8s scripted mood+colour diagnostics
# emo2 state signals.
CTRL_TOKEN_EXPIRED = 0x18
CTRL_TOKEN_RECOVERED = 0x19
CTRL_MANUAL_ON = 0x1A
CTRL_MANUAL_OFF = 0x1B
# emo2 halo colour override.
CTRL_COLOR_CYAN  = 0x1C
CTRL_COLOR_AMBER = 0x1D
CTRL_COLOR_RED   = 0x1E
CTRL_COLOR_AUTO  = 0x1F

# % stats layout pick (firmware emo2.cpp LAYOUT_*).
CTRL_STATS_OFF         = 0x20
CTRL_STATS_BEZEL       = 0x21
CTRL_STATS_COLUMNS     = 0x22
CTRL_STATS_RIBBON      = 0x23
CTRL_STATS_BROWS       = 0x24
CTRL_STATS_TEAR_PEARLS = 0x25
CTRL_STATS_CORNER_CHIP = 0x26
CTRL_STATS_ECG         = 0x27
CTRL_STATS_CLASSIC     = 0x28

# Display orientation (rotation 0..3 — currently only 0/1 exposed).
# Persisted on ESP via Preferences; web pill and BTN_B both update it.
CTRL_ROTATION_PORTRAIT  = 0x30
CTRL_ROTATION_LANDSCAPE = 0x31
ROTATION_CTRL = {
    "vertical":   CTRL_ROTATION_PORTRAIT,
    "horizontal": CTRL_ROTATION_LANDSCAPE,
}
STATS_LAYOUT_CTRL = {
    "none":         CTRL_STATS_OFF,
    "bezel_orbit":  CTRL_STATS_BEZEL,
    "twin_columns": CTRL_STATS_COLUMNS,
    "hud_ribbon":   CTRL_STATS_RIBBON,
    # CTRL_STATS_BROWS (0x24) intentionally not mapped — deprecated layout,
    # firmware enum kept for compat but renders nothing.
    "tear_pearls":  CTRL_STATS_TEAR_PEARLS,
    "corner_chip":  CTRL_STATS_CORNER_CHIP,
    "ecg_monitor":  CTRL_STATS_ECG,
    # LAYOUT_CLASSIC — the legacy "text under eyes + thin horizontal bars"
    # rendering, now exposed as a proper layout choice so it stops fighting
    # with the bezel/columns/ribbon for screen space.
    "classic":      CTRL_STATS_CLASSIC,
}

# Usage text mode (firmware emo2.cpp TEXT_MODE_*).
CTRL_TEXT_NONE   = 0x44
CTRL_TEXT_PCT    = 0x45
CTRL_TEXT_RESET  = 0x46
CTRL_TEXT_BOTH   = 0x47
TEXT_MODE_CTRL = {
    "none":  CTRL_TEXT_NONE,
    "pct":   CTRL_TEXT_PCT,
    "reset": CTRL_TEXT_RESET,
    "both":  CTRL_TEXT_BOTH,
}
TEXT_MODES = tuple(TEXT_MODE_CTRL.keys())

# Clock styles — 8 picked candidates (c01/c05/c07/c08/c10/c12/c13/c15 from
# emo2-clock-candidates.html) + off. Firmware enum CS_* in emo2.cpp.
CTRL_CLOCK_OFF        = 0x48
CTRL_CLOCK_MONO       = 0x49   # c01 — Share Tech Mono · white · big
CTRL_CLOCK_MAJOR_MONO = 0x4A   # c05 — Major Mono Display · cyan
CTRL_CLOCK_ORBITRON   = 0x4B   # c07 — Orbitron 700 · widely-spaced cyan
CTRL_CLOCK_OUTLINE    = 0x4C   # c08 — Outline approximation
CTRL_CLOCK_NEON       = 0x58   # c10 — Cyan soft glow
# (Badge / c12 dropped — user rejected the look. Byte 0x59 reserved.)
CTRL_CLOCK_SECONDS    = 0x5A   # c13 — HH:MM:SS · seconds visible
CTRL_CLOCK_BRACKET    = 0x5B   # c15 — [HH:MM] · amber inside dim brackets
CLOCK_STYLE_CTRL = {
    "off":        CTRL_CLOCK_OFF,
    "mono":       CTRL_CLOCK_MONO,
    "major_mono": CTRL_CLOCK_MAJOR_MONO,
    "orbitron":   CTRL_CLOCK_ORBITRON,
    "outline":    CTRL_CLOCK_OUTLINE,
    "neon":       CTRL_CLOCK_NEON,
    "seconds":    CTRL_CLOCK_SECONDS,
    "bracket":    CTRL_CLOCK_BRACKET,
}
CLOCK_STYLES = tuple(CLOCK_STYLE_CTRL.keys())
# Legacy v1 → v2 mapping (user-picked closest visual equivalent).
_CLOCK_STYLE_LEGACY_MAP = {
    "minimal": "outline",      # both small/thin
    "big":     "mono",         # both large white
    "dot":     "orbitron",     # both letter-spaced
    "badge":   "neon",         # both bright/highlighted (badge dropped 2026-05)
}

# Layout-fill palette override (independent from halo). Locks every %-layout
# fill to a fixed colour, or 'auto' to keep the cyan→amber→red pct lerp.
CTRL_LAYOUT_COLOR_CYAN  = 0x4D
CTRL_LAYOUT_COLOR_AMBER = 0x4E
CTRL_LAYOUT_COLOR_RED   = 0x4F
CTRL_LAYOUT_COLOR_AUTO  = 0x50
LAYOUT_COLOR_CTRL = {
    "cyan":  CTRL_LAYOUT_COLOR_CYAN,
    "amber": CTRL_LAYOUT_COLOR_AMBER,
    "red":   CTRL_LAYOUT_COLOR_RED,
    "auto":  CTRL_LAYOUT_COLOR_AUTO,
}
LAYOUT_COLORS = tuple(LAYOUT_COLOR_CTRL.keys())

# Validation regex for custom-hex colour values. Per-state per-state.color
# AND top-level layout_color may now be a "#RRGGBB" string in addition to
# the named built-ins. The CTRL byte path stays palette-only (no CTRL for
# custom colours) — custom values land on the ESP via the cfg JSON push
# which carries the hex straight through.
import re as _re
HEX_COLOR_RE = _re.compile(r"^#[0-9A-Fa-f]{6}$")
def is_color_value(v: object) -> bool:
    if not isinstance(v, str):
        return False
    if v in ("auto", "cyan", "amber", "red"):
        return True
    return bool(HEX_COLOR_RE.match(v))

# Per-layout text source (which value(s) to show) — separate from text mode
# format above. Source = off/session/weekly/both. Migration: legacy text_mode
# values map to (source, format) — see _migrate_text_modes() below.
CTRL_TEXT_SRC_OFF  = 0x51
CTRL_TEXT_SRC_SESS = 0x52
CTRL_TEXT_SRC_WEEK = 0x53
CTRL_TEXT_SRC_BOTH = 0x54
TEXT_SOURCE_CTRL = {
    "off":     CTRL_TEXT_SRC_OFF,
    "session": CTRL_TEXT_SRC_SESS,
    "weekly":  CTRL_TEXT_SRC_WEEK,
    "both":    CTRL_TEXT_SRC_BOTH,
}
TEXT_SOURCES = tuple(TEXT_SOURCE_CTRL.keys())

# Per-layout text format — what's inside the visible text(s).
CTRL_TEXT_FMT_PCT   = 0x55
CTRL_TEXT_FMT_BOTH  = 0x56     # pct + reset time
CTRL_TEXT_FMT_RESET = 0x57
TEXT_FORMAT_CTRL = {
    "pct":       CTRL_TEXT_FMT_PCT,
    "pct_reset": CTRL_TEXT_FMT_BOTH,
    "reset":     CTRL_TEXT_FMT_RESET,
}
TEXT_FORMATS = tuple(TEXT_FORMAT_CTRL.keys())

# Per-layout text placement — which Y-band the overlay text(s) sit in.
# Three variants split the lower screen block into thirds (top/middle/bottom).
# Firmware uses INFO_*_Y[index] + BAR_*_Y[index] arrays keyed by this value.
CTRL_TEXT_PLACE_TOP    = 0x5C
CTRL_TEXT_PLACE_MIDDLE = 0x5D
CTRL_TEXT_PLACE_BOTTOM = 0x5E
TEXT_PLACEMENT_CTRL = {
    "top":    CTRL_TEXT_PLACE_TOP,
    "middle": CTRL_TEXT_PLACE_MIDDLE,
    "bottom": CTRL_TEXT_PLACE_BOTTOM,
}
TEXT_PLACEMENTS = tuple(TEXT_PLACEMENT_CTRL.keys())

# Per-layout placement allowed-set (item 4 constraint matrix). Empty tuple
# means the layout has its own placement scheme (e.g. chip uses fixed
# corner positions) and the placement picker is hidden in the web UI.
LAYOUT_PLACEMENT_ALLOWED = {
    "bezel_orbit":  ("top", "middle", "bottom"),
    "twin_columns": ("top", "middle", "bottom"),
    "hud_ribbon":   ("top", "middle", "bottom"), # item 4.1 (refined): bottom enabled — firmware maps it to middle's Y (where the text must sit to stay above the ribbon)
    "corner_chip":  (),                          # item 4.2: chip has own layout
    "ecg_monitor":  ("bottom",),                 # item 4.3: only bottom (below trace)
    "classic":      ("middle",),                 # item 4.4: only middle (locked)
    "tear_pearls":  ("top", "middle", "bottom"),
    "none":         (),                          # off — settings hidden entirely
}

CTRL_REFRESH = 0x10
CTRL_LANG_EN = 0x40        # connect-time seed (firmware ignores if user set)
CTRL_LANG_RU = 0x41
CTRL_LANG_EN_SET = 0x42    # explicit choice — firmware applies + saves to NVS
CTRL_LANG_RU_SET = 0x43

SCREEN_MAP = {
    "usage": CTRL_SCREEN_USAGE,
    "bluetooth": CTRL_SCREEN_BT,
    "bt": CTRL_SCREEN_BT,
    "splash": CTRL_SCREEN_SPLASH,
    "cycle": CTRL_SCREEN_CYCLE,
    "emo": CTRL_SCREEN_EMO,
    "emo2": CTRL_SCREEN_EMO2,
}

LANG_MAP = {"en": CTRL_LANG_EN, "ru": CTRL_LANG_RU}
LANG_SET_MAP = {"en": CTRL_LANG_EN_SET, "ru": CTRL_LANG_RU_SET}

ACTION_MAP = {
    "reboot": CTRL_REBOOT,
    "view": CTRL_CYCLE_VIEW,
    "anim": CTRL_NEXT_ANIM,
    "diagnose": CTRL_DIAGNOSE,
    # emo2 halo colour overrides (also drive the Preview swatches).
    "color_cyan":  CTRL_COLOR_CYAN,
    "color_amber": CTRL_COLOR_AMBER,
    "color_red":   CTRL_COLOR_RED,
    "color_auto":  CTRL_COLOR_AUTO,
}

# emo2 explicit form select — order must match mood_t in emo2.cpp.
FORM_NAMES = ["happy", "neutral", "sleep", "angry", "upset", "sad", "love",
              "circle", "pupil_left", "pupil_slit", "cross",
              "oval_tall", "diamond",
              "squircle_thick", "rect_tv", "capsule_h", "crescent",
              "brackets", "pixel_cluster", "q_eye", "exclaim"]
# Form CTRL range moved to 0x80..0x9F (32 slots, 13 forms today). The old
# base 0x50 collided with the new clock/text/layout-colour CTRLs
# (0x4D-0x5B); clicking a Forms tile would accidentally fire e.g.
# `emo2_set_text_source(WEEKLY)` for form #1.
for _i, _n in enumerate(FORM_NAMES):
    ACTION_MAP[f"form_{_n}"] = 0x80 + _i

# emo2 explicit operation trigger — order must match DiagMotion in emo2.cpp.
OP_NAMES = ["blink", "wink", "saccade", "eyeroll", "shake", "confused",
            "surprise", "bounce", "nod", "wave", "pulse_alt", "yawn",
            "warning"]
# Op CTRL range moved to 0xA0..0xBF (was 0x70..0x7F which is fine, but
# keeping forms+ops in adjacent ranges above 0x7F keeps future settings
# free below). DiagMotion starts at DM_BLINK=1 (DM_NONE=0).
for _i, _n in enumerate(OP_NAMES):
    ACTION_MAP[f"op_{_n}"] = 0xA0 + 1 + _i

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0
HTTP_PORT = 8765

LANG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "lang"
# Last web-chosen display orientation. Mirrors LANG_FILE (host-side memory of a
# device-NVS-owned setting) so a page reload restores the right aspect.
ORIENT_FILE = Path.home() / ".config" / "claude-usage-monitor" / "orient"
EMO2_CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "emo2_states.json"
# % layout picks (picked in emo2-stats.html gallery). One default + 0..N extras.
EMO2_STATS_FILE = Path.home() / ".config" / "claude-usage-monitor" / "emo2_stats.json"
# Persisted OAuth token (paste-from-web flow). Acts as CLAUDE_CODE_OAUTH_TOKEN
# without requiring the user to wrangle shell export + daemon restart.
OAUTH_TOKEN_FILE = Path.home() / ".config" / "claude-usage-monitor" / "oauth_token"

# Valid % layout IDs (gallery superset — covers experimental options not
# yet rendered on firmware). Firmware-implemented subset:
EMO2_STATS_LAYOUTS_FW = ("none", "bezel_orbit", "twin_columns", "hud_ribbon",
                          "tear_pearls", "corner_chip", "ecg_monitor", "classic")
# Gallery-only IDs (web preview supports them but firmware has no LAYOUT_*):
EMO2_STATS_LAYOUTS = EMO2_STATS_LAYOUTS_FW + (
    "pupil_gauge", "pulse_rate", "constellation",
)

# User's picks from gallery.
# Schema v3:
#   {default, extras, active, layout_color, clock_style,
#    layouts: {<id>: {text_source, text_format}}}
# v1 (top-level text_mode) and v2 (per-layout text_mode='both/pct/reset/none')
# are migrated on first load — see _migrate_text_modes().
DEFAULT_EMO2_STATS = {
    "default": "bezel_orbit",
    "extras": ["twin_columns", "hud_ribbon", "corner_chip", "ecg_monitor", "tear_pearls"],
    "active":  "bezel_orbit",
    "layout_color": "auto",          # cyan / amber / red / auto (lerp by pct)
    "clock_style":  "off",           # off / mono / major_mono / orbitron /
                                     # outline / neon / badge / seconds / bracket
    "clock_color":  "default",       # default = per-style hardcoded colour (white
                                     # / cyan / etc as defined in firmware
                                     # apply_clock_style); auto = follow the live
                                     # %-gradient; "#RRGGBB" = one custom colour.
    # Pace multipliers (×10, range 5..50 → 0.5×..5×). 20 = 2× (animations
    # and form rotations are noticeably slower than the legacy 10 = 1×).
    "anim_pace_x10": 20,
    "form_pace_x10": 20,
    # User-editable colour-by-% gradient. 2..4 stops; lerp linearly between
    # consecutive (pct, color) pairs. Firmware persists in NVS and reuses
    # for halo + every layout fill.
    # STEP-function thresholds — colour SNAPS to the next bright preset when
    # session_pct crosses each `pct`. Saturated palette + low thresholds so
    # the colour reacts within a typical session (most session_pct values
    # sit in 20-60%; older 0/50/70/90 set kept everything at first-stop
    # cyan for entire sessions and read as "color-by-% not applied").
    "color_stops": [
        {"pct": 0,  "color": "#00E5E5"},   # bright cyan  (calm, 0..20)
        {"pct": 20, "color": "#00FF66"},   # vivid green  (productive, 20..50)
        {"pct": 50, "color": "#FFD200"},   # saturated yellow (warning, 50..80)
        {"pct": 80, "color": "#FF1818"},   # pure red     (alert, 80..100)
    ],
    # Interpolation mode for the colour gradient.
    #   "step"   — pick the highest stop whose pct ≤ session_pct (firmware
    #              default; abrupt threshold crossings, "alarm" feel).
    #   "smooth" — linear lerp between adjacent stops (continuous tint, more
    #              decorative). Set via the master gradient-editor toggle.
    "gradient_mode": "step",
    # Schema version for color_stops. Bumped to 2 when thresholds moved from
    # 0/50/70/90 → 0/20/50/80. Stored configs with _stops_v < 2 (or absent)
    # are migrated by ignoring the persisted stops and re-applying the new
    # defaults — so existing users see the responsive palette without
    # touching the gradient editor manually.
    "_stops_v": 2,
    "layouts": {
        "bezel_orbit":  {"text_source": "both",    "text_format": "pct_reset", "text_placement": "middle"},
        "twin_columns": {"text_source": "both",    "text_format": "pct",       "text_placement": "middle"},
        "hud_ribbon":   {"text_source": "off",     "text_format": "pct",       "text_placement": "middle"},
        "corner_chip":  {"text_source": "both",    "text_format": "pct",       "text_placement": "middle"},
        "ecg_monitor":  {"text_source": "off",     "text_format": "pct",       "text_placement": "bottom"},
        "tear_pearls":  {"text_source": "both",    "text_format": "pct_reset", "text_placement": "middle"},
        "classic":      {"text_source": "both",    "text_format": "pct_reset", "text_placement": "middle"},
        "none":         {"text_source": "off",     "text_format": "pct",       "text_placement": "middle"},
    },
}

# v2 → v3 migration table: old per-layout `text_mode` field decomposed into
# (text_source, text_format) pair. Used in _migrate_text_modes() during load
# and in _handle_emo2_stats_config when accepting legacy POSTs.
_LEGACY_TEXT_MODE_MAP = {
    "none":  ("off",  "pct"),
    "pct":   ("both", "pct"),
    "reset": ("both", "reset"),
    "both":  ("both", "pct_reset"),
}

# Colour-by-% gradient validator/normaliser. Accepts a list of {pct, color}
# dicts, returns a sanitized list sorted by pct, or None on any structural
# error. min 2, max 4 stops; pct ∈ [0..100]; color = "#RRGGBB".
def _validate_color_stops(raw):
    if not isinstance(raw, list):
        return None
    if not (2 <= len(raw) <= 4):
        return None
    out = []
    for item in raw:
        if not isinstance(item, dict):
            return None
        pct = item.get("pct")
        col = item.get("color")
        if not isinstance(pct, (int, float)) or not isinstance(col, str):
            return None
        if pct < 0 or pct > 100:
            return None
        col = col.strip()
        # Accept "#RRGGBB" or "#RGB" (expand to RRGGBB).
        if not (col.startswith("#") and len(col) in (4, 7)):
            return None
        body = col[1:]
        try:
            int(body, 16)
        except ValueError:
            return None
        if len(body) == 3:
            body = "".join(c * 2 for c in body)
        out.append({"pct": int(round(pct)), "color": "#" + body.upper()})
    out.sort(key=lambda x: x["pct"])
    return out


def _load_persisted_oauth_token() -> None:
    """Read OAUTH_TOKEN_FILE into env-var if file exists and env not already
    set by the shell. Called once at daemon startup so the paste-from-web
    token survives restarts."""
    if os.environ.get("CLAUDE_CODE_OAUTH_TOKEN"):
        return  # shell env wins
    if not OAUTH_TOKEN_FILE.exists():
        return
    try:
        tok = OAUTH_TOKEN_FILE.read_text().strip()
        if tok:
            os.environ["CLAUDE_CODE_OAUTH_TOKEN"] = tok
            log(f"Loaded persisted OAuth token from {OAUTH_TOKEN_FILE}")
    except Exception as e:
        log(f"Failed to read {OAUTH_TOKEN_FILE}: {e}")

# emo2 state configuration. Each state has a list of forms / ops to rotate
# through and a colour mode. The daemon pushes the right CTRL bytes when
# the active state changes or its rotation timer fires.
DEFAULT_EMO2_CONFIG = {
    # Phase D: per-state config carries the per-state visual stack —
    # forms / ops / color (halo) + text_source + text_format +
    # text_placement + layout_color picks. Layout is GLOBAL (single
    # `layouts.active` picker), no longer per-state, so each device state
    # (connected / connecting / ble_off / token_expired) presents distinct
    # content/colour but shares one layout.
    "connected":     {"forms": ["neutral", "circle", "happy"],
                      "ops":   ["blink", "wave"],
                      "color": "auto",
                      "text_source": "both", "text_format": "reset",
                      "text_placement": "middle", "layout_color": "auto"},
    "connecting":    {"forms": ["circle"],
                      "ops":   ["pulse_alt"],
                      "color": "cyan",
                      "text_source": "off", "text_format": "pct",
                      "text_placement": "middle", "layout_color": "auto"},
    "ble_off":       {"forms": ["cross", "upset"],
                      "ops":   ["shake"],
                      "color": "red",
                      "text_source": "off", "text_format": "pct",
                      "text_placement": "middle", "layout_color": "amber"},
    "token_expired": {"forms": ["cross", "angry"],
                      "ops":   ["warning"],
                      "color": "red",
                      "text_source": "off", "text_format": "pct",
                      "text_placement": "middle", "layout_color": "red"},
}
EMO2_STATES = list(DEFAULT_EMO2_CONFIG.keys())
EMO2_COLOR_CTRL = {"cyan": 0x1C, "amber": 0x1D, "red": 0x1E, "auto": 0x1F}
# Form/op rotation timers moved to ESP (Phase A) — emo2.cpp EMO2_FORM_ROT_MS / OP_ROT_MS.
SDK_CREDENTIALS_DIR = Path.home() / ".config" / "anthropic"
WEB_DIR = Path(__file__).parent / "web"

# Public Claude Code OAuth client ID — the SDK's CredentialsFile provider sends
# this verbatim in the refresh_token grant. It must be the client UUID, not the
# client-metadata URL (a URL here makes the token endpoint reject the refresh
# with HTTP 400 "Invalid request format").
CLAUDE_CODE_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


# ---------------------------------------------------------------------------
# File logger — rotates at LOG_MAX_BYTES, keeps LOG_KEEP archives. Path under
# the same XDG config dir so all daemon state lives in one place.
# ---------------------------------------------------------------------------
LOG_FILE       = Path.home() / ".config" / "claude-usage-monitor" / "daemon.log"
LOG_MAX_BYTES  = 1_000_000          # 1 MB before rotation
LOG_KEEP       = 3                  # keep daemon.log.1 / .2 / .3
_log_lock      = threading.Lock()
_log_inited    = False

def _ensure_log_dir():
    global _log_inited
    if _log_inited:
        return
    try:
        LOG_FILE.parent.mkdir(parents=True, exist_ok=True)
        _log_inited = True
    except Exception:
        # Read-only home or similar — log() will silently no-op the file write.
        _log_inited = True

def _rotate_log():
    try:
        if not LOG_FILE.exists() or LOG_FILE.stat().st_size < LOG_MAX_BYTES:
            return
        # daemon.log.2 → .3, .1 → .2, current → .1.
        for i in range(LOG_KEEP - 1, 0, -1):
            old = LOG_FILE.parent / f"daemon.log.{i}"
            new = LOG_FILE.parent / f"daemon.log.{i+1}"
            if old.exists():
                old.replace(new)
        LOG_FILE.replace(LOG_FILE.parent / "daemon.log.1")
    except Exception:
        pass  # rotation is best-effort; don't crash daemon

def log(msg: str) -> None:
    # Higher-resolution timestamp than the previous H:M:S so reconnect-storm
    # timing is debuggable from the log file alone.
    ts = time.strftime("%H:%M:%S")
    ms = int((time.time() % 1) * 1000)
    line = f"[{ts}.{ms:03d}] {msg}"
    print(line, flush=True)
    _ensure_log_dir()
    try:
        with _log_lock:
            _rotate_log()
            with LOG_FILE.open("a", encoding="utf-8") as f:
                f.write(line + "\n")
    except Exception:
        pass  # file-system error must never crash the daemon


# ---------------------------------------------------------------------------
# Daemon state — shared between BLE loop and HTTP server
# ---------------------------------------------------------------------------

@dataclass
class DaemonState:
    """Shared mutable state accessible from both BLE and HTTP contexts."""
    ble_connected: bool = False
    ble_address: Optional[str] = None
    # Reconnect telemetry — populated by connect_and_run / disconnect handler.
    # Used by both the daemon.log file and the /api/diag endpoint so the user
    # can see WHEN reconnects happen and pattern-match against device-side
    # events (boot, OOM, watchdog).
    ble_connect_count:    int   = 0    # cumulative connects since daemon start
    ble_disconnect_count: int   = 0    # cumulative disconnects
    ble_last_connect_ms:  float = 0.0  # time.time() on last successful connect
    ble_last_disconnect_ms: float = 0.0
    ble_last_uptime_s:    float = 0.0  # duration of LAST connection (seconds)
    ble_recent_uptimes:   list  = field(default_factory=list)  # last 10 uptimes
    last_payload: Optional[dict] = None
    last_poll_time: float = 0.0
    start_time: float = field(default_factory=time.time)
    poll_interval: int = POLL_INTERVAL
    firmware_lang: str = "en"
    # Last web-chosen display orientation ("vertical"|"horizontal"). The
    # firmware's NVS is the real source of truth (daemon can't read it back),
    # but tracking the last web pick lets a page reload restore the correct
    # aspect + placement-allowed set instead of always defaulting to portrait.
    orientation: str = "vertical"
    ctrl_queue: asyncio.Queue = field(default_factory=asyncio.Queue)
    ws_clients: set = field(default_factory=set)
    api_client: Optional[Anthropic] = None
    auth_error: bool = False
    auth_retry_count: int = 0            # consecutive failed Keychain re-reads
    force_poll: bool = False             # set by /api/oauth-token to poll asap
    manual_mode: bool = False           # Settings → manual override
    emo2_config: dict = field(default_factory=lambda: {
        k: dict(v) for k, v in DEFAULT_EMO2_CONFIG.items()
    })
    emo2_state: str = "ble_off"         # current daemon-derived state
    # Rotation indices used to live here (form_i/op_i/entered_ms) — moved to
    # ESP-side state machine (Phase A). emo2_state still tracked for the
    # web WS broadcast of the ACTIVE-tab indicator.
    emo2_stats: dict = field(default_factory=lambda: dict(DEFAULT_EMO2_STATS))
    session: Optional["Session"] = None     # live BLE session (None when disconnected)
    ota_in_progress: bool = False           # one OTA at a time


# ---------------------------------------------------------------------------
# SDK-backed credential provider
# ---------------------------------------------------------------------------

def _sync_keychain_to_sdk_credentials(force: bool = False) -> bool:
    creds_path = SDK_CREDENTIALS_DIR / "credentials" / "default.json"
    config_path = SDK_CREDENTIALS_DIR / "configs" / "default.json"

    # `force` bypasses the freshness short-circuit: after an invalid_grant the
    # access token may still look unexpired by clock yet be revoked server-side.
    # Re-reading the Keychain unconditionally lets the daemon auto-recover the
    # moment the user re-authenticates via the Claude Code app/CLI.
    if not force and creds_path.exists():
        try:
            data = json.loads(creds_path.read_text())
            expires_at = data.get("expires_at", 0)
            if expires_at and time.time() < expires_at - 300:
                return True
        except (json.JSONDecodeError, OSError):
            pass

    import getpass
    try:
        out = subprocess.run(
            ["security", "find-generic-password", "-s", "Claude Code-credentials",
             "-a", getpass.getuser(), "-w"],
            check=True, capture_output=True, text=True, timeout=10,
        )
        blob = out.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain read failed: {e}")
        return False

    try:
        d = json.loads(blob)
    except json.JSONDecodeError:
        log("Keychain credential is not valid JSON")
        return False

    oauth = d.get("claudeAiOauth", d)
    access_token = oauth.get("accessToken", "")
    refresh_token = oauth.get("refreshToken", "")
    expires_at = oauth.get("expiresAt", 0)

    if expires_at > 1e12:
        expires_at = int(expires_at / 1000)

    if not access_token:
        log("No accessToken in Keychain credentials")
        return False

    config_path.parent.mkdir(parents=True, exist_ok=True)
    config = {
        "version": "1.0",
        "authentication": {
            "type": "user_oauth",
            "client_id": CLAUDE_CODE_CLIENT_ID,
        },
    }
    config_path.write_text(json.dumps(config, indent=2))

    creds_path.parent.mkdir(parents=True, exist_ok=True)
    creds = {
        "version": "1.0",
        "type": "oauth_token",
        "access_token": access_token,
        "refresh_token": refresh_token,
        "expires_at": expires_at,
    }
    creds_path.write_text(json.dumps(creds, indent=2))
    os.chmod(creds_path, 0o600)

    log(f"Synced Keychain → SDK credentials (expires_at={expires_at})")
    return True


def _create_anthropic_client(force: bool = False) -> Optional[Anthropic]:
    # Env-var takes precedence over Keychain. This is the "headless fix"
    # path: when a user does `export CLAUDE_CODE_OAUTH_TOKEN=...` they
    # expect that token to be used, even if a (stale) Keychain entry exists.
    env_token = os.environ.get("CLAUDE_CODE_OAUTH_TOKEN")
    if env_token:
        log("Using CLAUDE_CODE_OAUTH_TOKEN env-var (overrides Keychain)")
        return Anthropic(auth_token=env_token)

    if _sync_keychain_to_sdk_credentials(force=force):
        try:
            provider = CredentialsFile()
            return Anthropic(credentials=provider)
        except Exception as e:
            log(f"SDK CredentialsFile setup failed: {e}")

    log("No credentials available — API calls will fail")
    return None


# ---------------------------------------------------------------------------
# API polling
# ---------------------------------------------------------------------------

async def poll_api(client: Anthropic, state: DaemonState) -> Optional[dict]:
    # Headless override: if an env-var bearer is provided, use it directly.
    # Skip the SDK's refresh-token cache entirely (the client may not even
    # have one when constructed with auth_token=...).
    env_token = os.environ.get("CLAUDE_CODE_OAUTH_TOKEN")
    if env_token:
        token = env_token
    else:
        try:
            token = await asyncio.to_thread(client._token_cache.get_token)
        except Exception as e:
            msg = str(e)
            if not state.auth_error:  # log the actionable hint once per outage
                if "invalid_grant" in msg:
                    log("AUTH: token expired — open Claude Code and re-login "
                        "(daemon auto-recovers ≤60s).")
                else:
                    log(f"Token acquisition failed: {e}")
            state.auth_error = True
            return None
    # Note: don't clear state.auth_error here — token acquisition succeeding
    # doesn't mean the API will accept it (e.g. revoked server-side). We clear
    # only after a successful API response below.

    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"

    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None

    if resp.status_code == 401:
        if not state.auth_error:
            log("AUTH: API returned 401 — token revoked or expired. "
                "Open Claude Code and re-login (daemon auto-recovers ≤60s), "
                "or paste a fresh long-lived token via the web banner.")
        state.auth_error = True
        # NOTE: we DON'T touch OAUTH_TOKEN_FILE here even if env_token was the
        # culprit. Auto-deleting the persisted file caused "пасту слетает":
        # one transient 401 at daemon startup nuked the user's saved token,
        # forcing them to re-paste after every restart. A pasted token via
        # the web overwrites the file regardless, so leaving it alone here
        # has no downside — and if the token's genuinely dead the user just
        # sees the banner and re-pastes. We still invalidate the SDK token
        # cache so any Keychain-based refresh path retries fresh.
        try:
            await asyncio.to_thread(client._token_cache.invalidate)
        except Exception:
            pass
        return None

    if resp.status_code != 200:
        log(f"API returned HTTP {resp.status_code} — skipping send")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    lt = time.localtime(now)
    tz_off = -(time.altzone if lt.tm_isdst else time.timezone)

    # API accepted the token. If we were in an auth-error state, clear it now —
    # this is the only place we know the FULL chain (token + server) works.
    if state.auth_error:
        state.auth_error = False
        log("AUTH: recovered — fresh credentials accepted by API")

    return {
        "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ts": int(now),
        "tz": tz_off,
        "ok": True,
    }


# ---------------------------------------------------------------------------
# BLE
# ---------------------------------------------------------------------------

async def scan_for_device():
    """Return a freshly-discovered BLEDevice (not a cached address).

    macOS/CoreBluetooth requires the peripheral to be discovered via a scan
    in the *current* process before BleakClient can connect to it. Caching
    the address and connecting directly fails on the first daemon start
    (hence the old "needs 2 restarts" behaviour) — so always scan.
    """
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d
    return None


class Session:
    def __init__(self, client: BleakClient, state: DaemonState) -> None:
        self.client = client
        self.state = state
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    def _on_tx(self, _char, data: bytearray) -> None:
        """TX-char notification from firmware. Two payload shapes today:

        - ``"[BOOT] reason=… free=… min=… up=…"`` — fires once per connect.
          Surfaces reset reason + heap stats in the daemon log so we can
          diagnose firmware panics / WDT / OOM crashes WITHOUT needing
          serial access. Sent by ble.cpp ble_tick() ~500 ms after connect.
        - ``"{\"ack\":true}"`` / ``"{\"err\":true}"`` — legacy ack/nack
          for cfg pushes (kept for back-compat; we just drop these so
          they don't spam the log).
        """
        try:
            txt = bytes(data).decode("utf-8", errors="replace").strip()
        except Exception:
            return
        if not txt:
            return
        if txt.startswith("[BOOT]") or txt.startswith("[HEAP]"):
            log(f"BLE: device telemetry → {txt}")
        # Ignore everything else (ack/err pings). If a future firmware
        # adds new TX payload prefixes, branch here.

    async def setup_refresh_subscription(self) -> None:
        # Subscribe to REQ (refresh) + TX (boot/heap telemetry) in one go.
        # Failures are logged but non-fatal — the daemon still works
        # without these subscriptions, just with reduced observability.
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")
        try:
            await self.client.start_notify(TX_CHAR_UUID, self._on_tx)
        except (BleakError, ValueError) as e:
            log(f"TX (telemetry) subscription unavailable: {e}")

    def _services_ok(self) -> bool:
        """True only if GATT service discovery has actually completed.

        On macOS/CoreBluetooth, after the peripheral resets (e.g. USB reflash
        or a watchdog reboot) bleak can keep ``is_connected == True`` while the
        previously-discovered services are gone. Every ``write_gatt_char`` then
        raises "Service Discovery has not been performed yet" — forever, because
        connect_and_run's ``while client.is_connected`` loop never exits to
        rescan. Accessing ``.services`` in that state raises, so guard it.
        """
        try:
            for _ in self.client.services:
                return True
            return False
        except Exception:
            return False

    async def _drop_for_reconnect(self, why: str) -> None:
        """Force-drop a half-up link so the outer loop rescans + reconnects.

        Disconnecting flips ``is_connected`` false → connect_and_run's loop
        exits → main scan loop reconnects with a FRESH service discovery,
        instead of failing every write indefinitely against a stale link.
        """
        log(f"BLE link unusable ({why}) — dropping to force a clean reconnect")
        try:
            await self.client.disconnect()
        except Exception:
            pass

    async def write_payload(self, payload: dict) -> bool:
        """Write a JSON payload to the ESP's RX char.

        Bleak's `response=False` writes are limited to (ATT_MTU - 3) bytes,
        which on macOS CoreBluetooth means ~182 bytes by default. Larger
        payloads silently get dropped before reaching the peripheral — that's
        what kept the full emo2 cfg blob (~720 B with states+layouts+color_
        stops) from ever arriving while the tiny test_pct push (24 B) worked.

        Protocol:
          - Small payloads (≤RX_CHUNK_BUDGET): single raw-JSON write,
            response=False — back-compat with the legacy single-shot path.
            Firmware's RX callback sees first byte = '{' and treats the
            whole frame as one JSON document.
          - Large payloads: chunked, write-with-response so we know each
            chunk was ACKed before sending the next:
              * First frame:       [0x01, total_lo, total_hi, json...]
              * Continuation frame:[0x02, json...]
            Firmware accumulates into rx_buf until rx_received == total,
            then triggers parse.
        Header bytes 0x01/0x02 are non-printable, so a legacy small JSON
        (which always starts with '{') can never be mistaken for a chunk.
        """
        data = json.dumps(payload, separators=(",", ":")).encode()
        # Pre-flight: a stale (connected-but-undiscovered) link would fail every
        # write below with "Service Discovery has not been performed yet". Catch
        # it here and force a reconnect instead of logging a Send + a failure.
        if not self.client.is_connected or not self._services_ok():
            await self._drop_for_reconnect("connected but no services discovered")
            return False
        log(f"Sending: {data.decode()}")
        try:
            if len(data) <= RX_CHUNK_BUDGET:
                await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
                return True
            # Large payload — chunk it. response=True ensures back-pressure
            # so we don't outrun the link layer.
            total = len(data)
            first_room = RX_CHUNK_BUDGET - 3
            first_pkt = bytes([0x01, total & 0xFF, (total >> 8) & 0xFF]) + data[:first_room]
            await self.client.write_gatt_char(RX_CHAR_UUID, first_pkt, response=True)
            offset = first_room
            cont_room = RX_CHUNK_BUDGET - 1
            while offset < total:
                chunk = data[offset:offset + cont_room]
                await self.client.write_gatt_char(
                    RX_CHAR_UUID, bytes([0x02]) + chunk, response=True
                )
                offset += len(chunk)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            if "service discovery" in str(e).lower():
                await self._drop_for_reconnect(str(e))
            return False

    async def write_ctrl(self, cmd: int) -> bool:
        """Send a control command byte to the ESP32."""
        # Same stale-link guard as write_payload — a half-up link silently
        # swallows CTRL bytes (orientation 0x30/0x31, lang, manual, …).
        if not self.client.is_connected or not self._services_ok():
            await self._drop_for_reconnect("connected but no services discovered")
            return False
        try:
            await self.client.write_gatt_char(CTRL_CHAR_UUID, bytes([cmd]), response=False)
            log(f"CTRL: sent 0x{cmd:02X}")
            return True
        except BleakError as e:
            log(f"CTRL write failed: {e}")
            if "service discovery" in str(e).lower():
                await self._drop_for_reconnect(str(e))
            return False

    async def ota_upload(self, blob: bytes) -> None:
        """Stream a firmware .bin over BLE OTA. Raises on error.

        Protocol mirrors firmware/src/ota.h:
          → write OTA_CTRL: 0x01 [size:4 LE]   wait for 0x10 READY
          → write OTA_DATA chunks (≤480 B, write-without-response)
          → write OTA_CTRL: 0x02 (END)          wait for 0x12 DONE
        Any 0xE0/0xE1/0xE2 notify aborts with the embedded esp_err code.
        Progress is broadcast on the WS as {type:'ota', status:'progress', ...}.
        """
        size = len(blob)
        log(f"OTA: starting upload, {size} bytes ({size/1024:.1f} KB)")

        # Pre-flight: link still up?
        if not self.client.is_connected:
            raise RuntimeError("BLE link dropped before OTA could start — reconnect and retry")

        # Pre-flight: does the device actually expose the OTA service? If not,
        # the device is running an older firmware without BLE OTA support.
        # Print clear message instead of an opaque "characteristic not found"
        # or "disconnected" from CoreBluetooth deeper in the stack.
        try:
            services = self.client.services
            ota_svc = None
            for svc in services:
                if str(svc.uuid).lower() == OTA_SERVICE_UUID.lower():
                    ota_svc = svc
                    break
            if ota_svc is None:
                raise RuntimeError(
                    "OTA service not present on device — current firmware predates "
                    "BLE-OTA support. Flash once via USB ("
                    "`scripts/build.sh --upload`) to install an OTA-capable build.")
        except RuntimeError:
            raise
        except Exception as e:
            log(f"OTA: service discovery check failed ({e}) — proceeding anyway")

        await _broadcast_ws(self.state, {"type": "ota", "status": "begin", "size": size})

        loop = asyncio.get_event_loop()
        evt = asyncio.Event()
        result = {"ready": False, "done": False, "err": None, "received": 0}
        last_ws_push = 0.0

        def schedule_progress_ws(received: int, total: int):
            asyncio.run_coroutine_threadsafe(
                _broadcast_ws(self.state,
                              {"type": "ota", "status": "progress",
                               "received": received, "size": total}),
                loop)

        def on_notify(_char, data: bytearray):
            nonlocal last_ws_push
            if not data:
                return
            st = data[0]
            if st == 0x10:  # READY
                result["ready"] = True
                evt.set()
            elif st == 0x11 and len(data) >= 5:  # PROGRESS
                received = int.from_bytes(data[1:5], "little")
                result["received"] = received
                now = time.monotonic()
                if now - last_ws_push >= 0.20:
                    last_ws_push = now
                    schedule_progress_ws(received, size)
            elif st == 0x12:  # DONE
                result["done"] = True
                evt.set()
            elif st in (0xE0, 0xE1, 0xE2):
                err = data[1] if len(data) >= 2 else 0
                kind = {0xE0: "begin", 0xE1: "write", 0xE2: "end"}[st]
                # Friendly mapping for the Arduino Update library error codes.
                # 9 = UPDATE_ERROR_ACTIVATE — esp_ota_end / set_boot failed,
                #     almost always because the image is incomplete (the BLE
                #     link dropped chunks). Daemon now uses write-with-response
                #     which should eliminate this.
                # 0xFE = custom "incomplete transfer" code from our ota.cpp.
                friendly = {
                    7:    "MD5 mismatch (image corrupted in transit)",
                    8:    "Magic byte missing (uploaded file isn't a valid ESP32 app image)",
                    9:    "Activate failed — image incomplete or partition mismatch",
                    10:   "No OTA partition found (partition table mismatch)",
                    11:   "Bad argument to Update library",
                    0xFD: "Stalled — firmware saw no chunk for 8s, aborted",
                    0xFE: "Incomplete transfer — bytes received < bytes promised",
                }.get(err, "")
                hint = f" — {friendly}" if friendly else ""
                result["err"] = f"esp_ota_{kind} error code {err}{hint}"
                evt.set()

        try:
            await self.client.start_notify(OTA_CTRL_CHAR_UUID, on_notify)
        except Exception as e:
            raise RuntimeError(
                f"Couldn't subscribe to OTA notifications ({e}). "
                "Likely the device firmware predates BLE-OTA support — "
                "flash via USB first with `scripts/build.sh --upload`.")
        try:
            # BEGIN
            begin_pkt = bytes([0x01,
                               size & 0xFF,
                               (size >> 8) & 0xFF,
                               (size >> 16) & 0xFF,
                               (size >> 24) & 0xFF])
            await self.client.write_gatt_char(OTA_CTRL_CHAR_UUID, begin_pkt, response=True)
            try:
                await asyncio.wait_for(evt.wait(), timeout=10.0)
            except asyncio.TimeoutError:
                raise RuntimeError("OTA: no READY notify after BEGIN (timeout)")
            if result["err"]:
                raise RuntimeError(result["err"])
            if not result["ready"]:
                raise RuntimeError("OTA: unexpected first notify")
            evt.clear()

            # Stream chunks. **Write-with-response** (response=True) — slower
            # than write-without-response (~25 KB/s vs ~80 KB/s) but the BLE
            # stack returns only after the peripheral ACKs each packet. The
            # write-without-response variant dropped packets occasionally,
            # which surfaced as `esp_ota_end error code 9`
            # (UPDATE_ERROR_ACTIVATE): the partition gets less data than
            # `Update.begin` was promised → esp_image_verify / set_boot_part
            # fails after the upload "completes." We trade speed for
            # reliability. A 1.5 MB firmware now takes ~60 s instead of ~20 s.
            # Per-chunk timeout — without it, a BLE stack stall (CoreBluetooth
            # not ACKing) would freeze the loop indefinitely with no signal to
            # the web UI. 5s is well above the worst-case 4KB-flash-erase
            # latency on the ESP-side `Update.write` (~50ms typical).
            CHUNK_TIMEOUT = 5.0
            chunks_total = (size + OTA_CHUNK_SIZE - 1) // OTA_CHUNK_SIZE
            log_every_bytes = 256 * 1024
            next_log_at = log_every_bytes
            sent = 0
            for offset in range(0, size, OTA_CHUNK_SIZE):
                chunk = blob[offset:offset + OTA_CHUNK_SIZE]
                try:
                    await asyncio.wait_for(
                        self.client.write_gatt_char(
                            OTA_DATA_CHAR_UUID, chunk, response=True),
                        timeout=CHUNK_TIMEOUT)
                except asyncio.TimeoutError:
                    # Try to abort cleanly on the device so it leaves OTA mode.
                    try:
                        await self.client.write_gatt_char(
                            OTA_CTRL_CHAR_UUID, bytes([0x03]), response=False)
                    except Exception:
                        pass
                    raise RuntimeError(
                        f"OTA: chunk write stalled at {sent}/{size} bytes "
                        f"(no ACK in {CHUNK_TIMEOUT:.1f}s — BLE link or "
                        f"peripheral flash stuck)")
                if result["err"]:
                    raise RuntimeError(result["err"])
                sent += len(chunk)
                if sent >= next_log_at:
                    pct = (sent * 100) // size if size else 0
                    log(f"OTA: {sent/1024:.1f} KB / {size/1024:.1f} KB ({pct}%)")
                    next_log_at += log_every_bytes
                # Cooperative yield so the notify-callback task runs.
                if (offset // OTA_CHUNK_SIZE) % 16 == 0:
                    await asyncio.sleep(0)

            # END
            await self.client.write_gatt_char(OTA_CTRL_CHAR_UUID, bytes([0x02]), response=True)
            try:
                await asyncio.wait_for(evt.wait(), timeout=15.0)
            except asyncio.TimeoutError:
                raise RuntimeError("OTA: no DONE notify after END (timeout)")
            if result["err"]:
                raise RuntimeError(result["err"])
            if not result["done"]:
                raise RuntimeError("OTA: no DONE received")

            log("OTA: upload complete, device will reboot")
            await _broadcast_ws(self.state, {"type": "ota", "status": "done"})
        finally:
            try:
                await self.client.stop_notify(OTA_CTRL_CHAR_UUID)
            except Exception:
                pass


async def connect_and_run(
    device, stop_event: asyncio.Event, state: DaemonState
) -> bool:
    address = getattr(device, "address", device)
    log(f"Connecting to {address}...")
    client = BleakClient(device)
    try:
        await client.connect()
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        state.ble_connected = False
        await _broadcast_ws(state, {"type": "ble", "state": "disconnected"})
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        state.ble_connected = False
        return False

    # Verify GATT service discovery actually completed. CoreBluetooth can
    # report a live connection (often a transparent re-link after the device
    # reset) without (re)discovering services; every subsequent write would
    # then raise "Service Discovery has not been performed yet" and the loop
    # below would never exit. Bail now so the outer scan loop reconnects clean.
    try:
        discovered = any(True for _ in client.services)
    except Exception:
        discovered = False
    if not discovered:
        log("Connected but GATT services not discovered — dropping for clean reconnect")
        try:
            await client.disconnect()
        except Exception:
            pass
        state.ble_connected = False
        return False

    # Reconnect telemetry — log enough context to diagnose "device keeps
    # reconnecting" bug reports without needing to attach serial. The recent
    # uptime list shows whether disconnects are clustering (e.g. every 30s
    # → BLE supervision timeout) or sparse (legit USB unplug / power blip).
    state.ble_connect_count += 1
    state.ble_last_connect_ms = time.time()
    gap_s = 0.0
    if state.ble_last_disconnect_ms > 0:
        gap_s = state.ble_last_connect_ms - state.ble_last_disconnect_ms
    log(f"BLE: CONNECT #{state.ble_connect_count} address={address} "
        f"gap_since_last_disconnect={gap_s:.1f}s "
        f"last_uptime={state.ble_last_uptime_s:.1f}s")
    state.ble_connected = True
    state.ble_address = address
    await _broadcast_ws(state, {"type": "ble", "state": "connected", "address": address})

    session = Session(client, state)
    state.session = session
    await session.setup_refresh_subscription()

    # Tell the firmware which UI language to render.
    await session.write_ctrl(LANG_MAP.get(state.firmware_lang, CTRL_LANG_EN))
    # Sync the manual + auth state on every fresh connection so the device
    # boots into the right visuals.
    # Always explicit-sync the token + manual state, not just the "on" case.
    # The ESP firmware persists `token_expired` / `manual_mode` across BLE
    # reconnects, so a daemon restart with a recovered token wouldn't clear
    # the "Re-auth needed" overlay unless we tell it to.
    await session.write_ctrl(CTRL_TOKEN_EXPIRED if state.auth_error else CTRL_TOKEN_RECOVERED)
    await session.write_ctrl(CTRL_MANUAL_ON    if state.manual_mode  else CTRL_MANUAL_OFF)
    # Phase B: instead of streaming individual CTRL bytes for layout/text
    # and then per-TICK form/op pushes, ship the whole config (per-state
    # forms/ops/colors + active layout + per-layout text mode) as one JSON
    # RX-write. ESP applies + persists to NVS — its local state machine
    # takes over rotation from here.
    await push_emo2_full_config(session, state)

    last_poll = 0.0
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            # Process control commands from HTTP/CLI
            while not state.ctrl_queue.empty():
                try:
                    cmd = state.ctrl_queue.get_nowait()
                    await session.write_ctrl(cmd)
                except asyncio.QueueEmpty:
                    break

            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or state.force_poll or elapsed >= state.poll_interval:
                session.refresh_requested.clear()
                state.force_poll = False
                was_auth_error = state.auth_error
                payload = await poll_api(state.api_client, state)
                if payload is not None:
                    if await session.write_payload(payload):
                        last_poll = time.time()
                        state.last_payload = payload
                        state.last_poll_time = last_poll
                        used_successfully = True
                        await _broadcast_ws(state, {"type": "data", "payload": payload})
                    else:
                        last_poll = time.time()
                    if was_auth_error:  # cleared inside poll_api on success
                        await _broadcast_ws(state, {"type": "auth", "ok": True})
                        await session.write_ctrl(CTRL_TOKEN_RECOVERED)
                        state.auth_retry_count = 0
                else:
                    last_poll = time.time()
                    if state.auth_error:
                        # Re-create the client forcing a Keychain re-read so a
                        # fresh `claude` login is picked up without a restart.
                        # Runs Keychain subprocess + file I/O — keep it off
                        # the event loop.
                        state.api_client = await asyncio.to_thread(
                            _create_anthropic_client, True)
                        if not was_auth_error:
                            await _broadcast_ws(state, {"type": "auth", "ok": False})
                            await session.write_ctrl(CTRL_TOKEN_EXPIRED)
                            state.auth_retry_count = 0
                        else:
                            state.auth_retry_count += 1
                            if state.auth_retry_count == 1:
                                log("AUTH: still expired after Keychain re-read. "
                                    "Fix attempt didn't reach the daemon. "
                                    "Open Claude Code → Sign out → Sign in, OR set "
                                    "CLAUDE_CODE_OAUTH_TOKEN env-var and restart the daemon.")

            # Re-derive emo2 state (cheap, no BLE write). On a real
            # transition the helper broadcasts to web; the ESP-side state
            # machine handles its own visuals from local signals + the
            # config blob we pushed on connect (Phase A → no per-TICK BLE
            # traffic from the daemon).
            emo2_state_changed_ws(state)

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        # Telemetry: compute this connection's uptime + reason hint.
        # `client.is_connected` flips false BEFORE this finally runs when the
        # remote peer initiated the drop; if still true here, it's a daemon-
        # side / stop_event close (CTRL-C, deliberate restart, USB unplug).
        now = time.time()
        uptime_s = 0.0
        if state.ble_last_connect_ms > 0:
            uptime_s = now - state.ble_last_connect_ms
        state.ble_disconnect_count += 1
        state.ble_last_disconnect_ms = now
        state.ble_last_uptime_s = uptime_s
        state.ble_recent_uptimes.append(uptime_s)
        if len(state.ble_recent_uptimes) > 10:
            state.ble_recent_uptimes.pop(0)
        # Reason hint — purely heuristic, gives "what likely happened" to
        # save grepping serial logs. Patterns observed so far:
        #   uptime < 5s   → device crashed / watchdog right after handshake
        #   5-30s         → BLE supervision timeout (link lost)
        #   30s+ no stop  → likely device reboot / firmware OTA
        #   stop_event    → daemon shutdown (clean)
        if stop_event.is_set():
            reason_hint = "daemon-stop"
        elif uptime_s < 5:
            reason_hint = "early-drop (firmware crash? watchdog?)"
        elif uptime_s < 30:
            reason_hint = "link-loss (supervision-timeout?)"
        elif client and not client.is_connected:
            reason_hint = "peer-initiated (device reboot / OOM?)"
        else:
            reason_hint = "unknown"
        avg = sum(state.ble_recent_uptimes) / max(1, len(state.ble_recent_uptimes))
        state.ble_connected = False
        state.session = None
        try:
            await asyncio.wait_for(client.disconnect(), timeout=3.0)
        except (BleakError, asyncio.TimeoutError, Exception):
            pass
        log(f"BLE: DISCONNECT #{state.ble_disconnect_count} uptime={uptime_s:.1f}s "
            f"recent_avg={avg:.1f}s hint=\"{reason_hint}\" "
            f"used_payload={used_successfully}")
        # Pattern flag: 3+ short uptimes (<15s) in a row = something is
        # wrong; surface as WARN so it's grep-able.
        if len(state.ble_recent_uptimes) >= 3:
            short = [u for u in state.ble_recent_uptimes[-3:] if u < 15]
            if len(short) >= 3:
                log("WARN: BLE flapping — 3 consecutive short uptimes (<15s). "
                    "Check ESP serial for crash / OOM / watchdog reset.")

    await _broadcast_ws(state, {"type": "ble", "state": "disconnected"})
    return used_successfully


# ---------------------------------------------------------------------------
# WebSocket broadcast
# ---------------------------------------------------------------------------

async def _broadcast_ws(state: DaemonState, msg: dict) -> None:
    if not state.ws_clients:
        return
    data = json.dumps(msg, separators=(",", ":"))
    dead = set()
    for ws in state.ws_clients:
        try:
            await ws.send_str(data)
        except Exception:
            dead.add(ws)
    state.ws_clients -= dead


# ---------------------------------------------------------------------------
# HTTP server
# ---------------------------------------------------------------------------

async def _handle_index(request: web.Request) -> web.Response:
    index_path = WEB_DIR / "index.html"
    if not index_path.exists():
        return web.Response(text="Web UI not found", status=404)
    return web.FileResponse(index_path)


async def _handle_status(request: web.Request) -> web.Response:
    state: DaemonState = request.app["state"]
    return web.json_response({
        "ble_connected": state.ble_connected,
        "ble_address": state.ble_address,
        "last_data": state.last_payload,
        "last_poll": state.last_poll_time,
        "poll_interval": state.poll_interval,
        "uptime": int(time.time() - state.start_time),
        "lang": state.firmware_lang,
        "auth_error": state.auth_error,
    })


async def _handle_diag(request: web.Request) -> web.Response:
    """Diagnostic dump — BLE reconnect counters + tail of daemon.log.
    Curl-friendly so support reports stay copy-pasteable:
       curl -s http://localhost:8765/api/diag | jq
    """
    state: DaemonState = request.app["state"]
    # Tail the last N lines from daemon.log without loading the whole file.
    tail_lines: list = []
    try:
        if LOG_FILE.exists():
            size = LOG_FILE.stat().st_size
            with LOG_FILE.open("rb") as f:
                # Read last 32 KB which comfortably holds ~400 typical lines.
                if size > 32_000:
                    f.seek(size - 32_000)
                chunk = f.read().decode("utf-8", errors="replace")
                tail_lines = chunk.splitlines()[-200:]
    except Exception as e:
        tail_lines = [f"<log read failed: {e}>"]
    now = time.time()
    return web.json_response({
        "uptime_s":        int(now - state.start_time),
        "ble": {
            "connected":          state.ble_connected,
            "address":            state.ble_address,
            "connect_count":      state.ble_connect_count,
            "disconnect_count":   state.ble_disconnect_count,
            "current_uptime_s":   round(now - state.ble_last_connect_ms, 1)
                                    if state.ble_connected and state.ble_last_connect_ms > 0 else 0,
            "last_uptime_s":      round(state.ble_last_uptime_s, 1),
            "recent_uptimes_s":   [round(u, 1) for u in state.ble_recent_uptimes],
            "recent_avg_s":       round(sum(state.ble_recent_uptimes) /
                                    max(1, len(state.ble_recent_uptimes)), 1),
        },
        "auth_error":    state.auth_error,
        "lang":          state.firmware_lang,
        "log_file":      str(LOG_FILE),
        "log_tail":      tail_lines,
    })


async def _handle_screen(request: web.Request) -> web.Response:
    state: DaemonState = request.app["state"]
    try:
        body = await request.json()
    except Exception:
        return web.json_response({"error": "invalid JSON"}, status=400)

    screen = body.get("screen", "").lower()
    cmd = SCREEN_MAP.get(screen)
    if cmd is None:
        return web.json_response(
            {"error": f"unknown screen '{screen}', valid: {list(SCREEN_MAP.keys())}"},
            status=400,
        )

    if not state.ble_connected:
        return web.json_response({"error": "BLE not connected"}, status=503)

    await state.ctrl_queue.put(cmd)
    return web.json_response({"ok": True, "screen": screen, "cmd": cmd})


async def _handle_refresh(request: web.Request) -> web.Response:
    state: DaemonState = request.app["state"]
    if not state.ble_connected:
        return web.json_response({"error": "BLE not connected"}, status=503)

    await state.ctrl_queue.put(CTRL_REFRESH)
    return web.json_response({"ok": True, "action": "refresh"})


async def _handle_emo2_config(request: web.Request) -> web.Response:
    """GET → return current per-state config + manual flag + state.
    POST → merge body into config, persist, re-push if state matches.
    Body shape:
      {"config": {"connected": {...}, ...}, "manual": true/false}
    Either key is optional."""
    state: DaemonState = request.app["state"]
    if request.method == "GET":
        return web.json_response({
            "config": state.emo2_config,
            "manual": state.manual_mode,
            "state":  state.emo2_state,
        })
    try:
        body = await request.json()
    except Exception:
        return web.json_response({"error": "invalid JSON"}, status=400)

    changed = False
    if "config" in body and isinstance(body["config"], dict):
        for k, v in body["config"].items():
            if k not in EMO2_STATES or not isinstance(v, dict):
                continue
            # Validate every potentially-stored field. Reject the whole
            # patch on first bad value so a malformed POST never lands
            # mixed garbage in NVS.
            if "color" in v and not is_color_value(v["color"]):
                return web.json_response(
                    {"error": f"{k}.color must be auto/cyan/amber/red or #RRGGBB, got {v['color']!r}"},
                    status=400)
            # Phase D: per-state fields (layout is GLOBAL now — not per-state).
            if "text_source" in v and v["text_source"] not in TEXT_SOURCES:
                return web.json_response(
                    {"error": f"{k}.text_source must be one of {list(TEXT_SOURCES)}"},
                    status=400)
            if "text_format" in v and v["text_format"] not in TEXT_FORMATS:
                return web.json_response(
                    {"error": f"{k}.text_format must be one of {list(TEXT_FORMATS)}"},
                    status=400)
            if "text_placement" in v and v["text_placement"] not in TEXT_PLACEMENTS:
                return web.json_response(
                    {"error": f"{k}.text_placement must be one of {list(TEXT_PLACEMENTS)}"},
                    status=400)
            if "layout_color" in v and not is_color_value(v["layout_color"]):
                return web.json_response(
                    {"error": f"{k}.layout_color must be auto/cyan/amber/red or #RRGGBB"},
                    status=400)
            state.emo2_config[k].update(v)
            changed = True
    if "manual" in body:
        new_manual = bool(body["manual"])
        if new_manual != state.manual_mode:
            state.manual_mode = new_manual
            # Send manual CTRL via direct write (NOT the dispatch queue which
            # has up to TICK=5s latency) so it lands BEFORE the config-blob
            # JSON push below. Without this ordering, ESP would receive the
            # new config while still in manual_mode → its state-machine
            # would skip rotation, user perceives "settings didn't apply".
            cmd = CTRL_MANUAL_ON if new_manual else CTRL_MANUAL_OFF
            if state.session is not None and state.ble_connected:
                try:
                    await state.session.write_ctrl(cmd)
                except Exception as e:
                    log(f"manual CTRL direct write failed: {e}")
            changed = True
    if changed:
        save_emo2_config(state.emo2_config)
        # Phase B: push the whole config blob in one JSON-write. ESP applies
        # + persists to NVS + force-applies the active state's visuals.
        if state.session is not None and state.ble_connected:
            await push_emo2_full_config(state.session, state)
    return web.json_response({"ok": True, "config": state.emo2_config,
                              "manual": state.manual_mode})


async def _handle_halo_live(request: web.Request) -> web.Response:
    """POST /api/halo-live  body: {"color": "<name>|#RRGGBB"}

    Sends a TRANSIENT halo-colour override to the ESP. Firmware applies
    immediately but does NOT save to NVS and does NOT touch the per-state
    config — so the next regular cfg push (e.g. on reconnect or when the
    user toggles manual mode off) reverts the eye to the per-state colour.

    Used by the web UI's Preview block when `manual_mode` is on, so the
    designer can A/B colours live without permanently changing any state's
    visuals. Built-ins (auto/cyan/amber/red) also work — same single channel.
    """
    state: DaemonState = request.app["state"]
    try:
        body = await request.json()
    except Exception:
        return web.json_response({"error": "invalid JSON"}, status=400)
    c = body.get("color")
    if not is_color_value(c):
        return web.json_response(
            {"error": "color must be auto/cyan/amber/red or '#RRGGBB'"},
            status=400)
    if not state.ble_connected or state.session is None:
        return web.json_response({"error": "BLE not connected"}, status=503)
    try:
        ok = await state.session.write_payload({"cfg": {"halo_live": c}})
    except Exception as e:
        return web.json_response({"error": str(e)}, status=500)
    return web.json_response({"ok": ok, "color": c})


async def _handle_action(request: web.Request) -> web.Response:
    """POST /api/action  body: {"action": "reboot"|"view"|"anim"}."""
    state: DaemonState = request.app["state"]
    try:
        body = await request.json()
    except Exception:
        return web.json_response({"error": "invalid JSON"}, status=400)

    action = str(body.get("action", "")).lower()
    cmd = ACTION_MAP.get(action)
    if cmd is None:
        return web.json_response(
            {"error": f"unknown action '{action}', valid: {list(ACTION_MAP.keys())}"},
            status=400,
        )

    if not state.ble_connected:
        return web.json_response({"error": "BLE not connected"}, status=503)

    # Attribute the action to its origin. Destructive bytes (esp. 0x05 reboot)
    # are otherwise indistinguishable in the log from a spontaneous device
    # reboot — logging the source here makes "why did it reboot?" answerable
    # from the log alone (it shows up just before the "CTRL: sent 0x05" line).
    log(f"ACTION: '{action}' (CTRL 0x{cmd:02X}) requested via HTTP from {request.remote or '?'}")
    await state.ctrl_queue.put(cmd)
    return web.json_response({"ok": True, "action": action, "cmd": cmd})


async def _handle_oauth_token(request: web.Request) -> web.Response:
    """POST /api/oauth-token  body: {"token": "sk-ant-oat01-..."}.

    Accepts a long-lived OAuth token (from `claude setup-token`) and uses it
    immediately as the bearer for API calls, bypassing the shell-export +
    daemon-restart dance. Persists to OAUTH_TOKEN_FILE so it survives restart.

    DELETE clears the persisted file (and env-var) and falls back to Keychain.
    """
    state: DaemonState = request.app["state"]

    if request.method == "DELETE":
        try:
            if OAUTH_TOKEN_FILE.exists():
                OAUTH_TOKEN_FILE.unlink()
        except Exception as e:
            log(f"Failed to delete {OAUTH_TOKEN_FILE}: {e}")
        os.environ.pop("CLAUDE_CODE_OAUTH_TOKEN", None)
        # Rebuild client from Keychain.
        state.api_client = await asyncio.to_thread(_create_anthropic_client, True)
        state.force_poll = True
        log("OAuth token cleared via web — falling back to Keychain.")
        return web.json_response({"ok": True, "source": "keychain"})

    try:
        body = await request.json()
    except Exception:
        return web.json_response({"error": "invalid JSON"}, status=400)

    token = str(body.get("token", "")).strip()
    if not token:
        return web.json_response({"error": "empty token"}, status=400)
    # Sanity-check: Claude Code OAuth tokens look like `sk-ant-oat01-...`.
    # Reject obvious noise (prompts, URLs, multi-line captures) early so the
    # user sees a clear error instead of a silent 401 loop.
    if " " in token or "\n" in token or len(token) < 40 or not token.startswith("sk-ant-"):
        return web.json_response({
            "error": "token doesn't look like a Claude OAuth token "
                     "(expected `sk-ant-oat01-...`, no spaces/newlines, ≥40 chars). "
                     "Run `claude setup-token` and paste ONLY the printed token line."
        }, status=400)

    # Persist before mutating env so a daemon crash during apply doesn't leave
    # us with an in-memory token that's gone on restart.
    try:
        OAUTH_TOKEN_FILE.parent.mkdir(parents=True, exist_ok=True)
        OAUTH_TOKEN_FILE.write_text(token)
        os.chmod(OAUTH_TOKEN_FILE, 0o600)
    except Exception as e:
        log(f"Failed to persist {OAUTH_TOKEN_FILE}: {e}")
        return web.json_response({"error": f"failed to persist token: {e}"}, status=500)

    os.environ["CLAUDE_CODE_OAUTH_TOKEN"] = token
    # Rebuild API client so the next poll uses the new token immediately.
    state.api_client = await asyncio.to_thread(_create_anthropic_client, True)
    state.auth_retry_count = 0
    state.force_poll = True
    log("OAuth token accepted via web — applying on next poll (≤5s).")
    return web.json_response({"ok": True, "source": "web"})


async def _handle_ota_upload(request: web.Request) -> web.Response:
    """POST /api/ota/upload  multipart: file=<.bin>.

    Streams the firmware to the ESP32 via the BLE OTA service. Progress is
    broadcast on the WS as {type:'ota', status:'progress', received, size}.
    On success the device commits + reboots; the BLE link drops, daemon
    reconnects within a few seconds.
    """
    state: DaemonState = request.app["state"]
    if not state.ble_connected or state.session is None:
        return web.json_response({"error": "BLE not connected"}, status=503)
    if state.ota_in_progress:
        return web.json_response({"error": "OTA already in progress"}, status=409)

    try:
        reader = await request.multipart()
    except Exception as e:
        return web.json_response({"error": f"not multipart: {e}"}, status=400)

    field = await reader.next()
    while field is not None and field.name != "file":
        field = await reader.next()
    if field is None:
        return web.json_response({"error": "missing 'file' multipart field"}, status=400)

    # Read entire firmware into memory (typical 1.5 MB, well under limits).
    blob = bytearray()
    while True:
        chunk = await field.read_chunk(65536)
        if not chunk:
            break
        blob += chunk
    if len(blob) < 4096:
        return web.json_response(
            {"error": f"firmware too small ({len(blob)} bytes)"}, status=400)
    # Cheap sanity: ESP32 image starts with magic 0xE9 (ESP_IMAGE_HEADER_MAGIC).
    if blob[0] != 0xE9:
        return web.json_response(
            {"error": "not an ESP32 firmware image (bad magic byte)"},
            status=400)

    state.ota_in_progress = True
    try:
        await state.session.ota_upload(bytes(blob))
    except Exception as e:
        log(f"OTA upload failed: {e}")
        await _broadcast_ws(state, {"type": "ota", "status": "error", "error": str(e)})
        return web.json_response({"error": str(e)}, status=500)
    finally:
        state.ota_in_progress = False

    return web.json_response({"ok": True, "size": len(blob)})


def load_emo2_config() -> dict:
    """Load per-state config from disk; merge with defaults so newly added
    states still work after a config-file upgrade."""
    merged = {k: dict(v) for k, v in DEFAULT_EMO2_CONFIG.items()}
    if EMO2_CONFIG_FILE.exists():
        try:
            stored = json.loads(EMO2_CONFIG_FILE.read_text())
            for k, v in stored.items():
                if k in merged and isinstance(v, dict):
                    merged[k].update(v)
        except Exception as e:
            log(f"emo2 config read failed, using defaults: {e}")
    return merged


def save_emo2_config(cfg: dict) -> None:
    try:
        EMO2_CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
        EMO2_CONFIG_FILE.write_text(json.dumps(cfg, indent=2))
    except OSError as e:
        log(f"Could not persist emo2 config: {e}")


def _apply_legacy_text_mode(layout_entry: dict, legacy: str) -> None:
    """v2 → v3: map a single 'text_mode' value to the (source, format) pair."""
    src, fmt = _LEGACY_TEXT_MODE_MAP.get(legacy, ("both", "pct_reset"))
    layout_entry["text_source"] = src
    layout_entry["text_format"] = fmt


def load_emo2_stats() -> dict:
    """Load % layout picks. Falls back to baked-in default when file is
    missing or malformed. Migrates schema:
      v1: top-level text_mode → v2 per-layout text_mode
      v2: per-layout text_mode='both/pct/reset/none' → v3 (text_source, text_format)
    """
    # Deep-copy so the in-memory default isn't mutated.
    cfg = {k: (v.copy() if isinstance(v, (dict, list)) else v)
           for k, v in DEFAULT_EMO2_STATS.items()}
    cfg["layouts"] = {k: dict(v) for k, v in DEFAULT_EMO2_STATS["layouts"].items()}
    if EMO2_STATS_FILE.exists():
        try:
            stored = json.loads(EMO2_STATS_FILE.read_text())
            d = stored.get("default")
            if isinstance(d, str) and d in EMO2_STATS_LAYOUTS:
                cfg["default"] = d
            ex = stored.get("extras")
            if isinstance(ex, list):
                cfg["extras"] = [x for x in ex
                                 if isinstance(x, str) and x in EMO2_STATS_LAYOUTS
                                 and x != cfg["default"]]
            a = stored.get("active")
            if isinstance(a, str) and a in EMO2_STATS_LAYOUTS:
                cfg["active"] = a
            else:
                cfg["active"] = cfg["default"]
            # Layout-fill colour (v3+). Accept built-in keywords AND a custom
            # "#RRGGBB" hex — `is_color_value` covers both. The old
            # `lc_val in LAYOUT_COLORS` test was keyword-only, so a custom hex
            # was silently dropped on reload (custom layout colour lost after
            # a daemon restart).
            lc_val = stored.get("layout_color")
            if is_color_value(lc_val):
                cfg["layout_color"] = lc_val
            # Clock style (v3+) — migrate legacy v1 names to v2 picks.
            cs_val = stored.get("clock_style")
            if isinstance(cs_val, str):
                cs_val = _CLOCK_STYLE_LEGACY_MAP.get(cs_val, cs_val)
                if cs_val in CLOCK_STYLES:
                    cfg["clock_style"] = cs_val
            # Colour-by-% gradient stops (v3+). Migration: ignore persisted
            # stops if their schema version is older than the current default
            # — the thresholds changed (0/50/70/90 → 0/20/50/80) and stale
            # stops kept the layout stuck on first-stop cyan for typical
            # session_pct ranges.
            stored_stops_v = stored.get("_stops_v", 1)
            cur_stops_v    = DEFAULT_EMO2_STATS.get("_stops_v", 1)
            if isinstance(stored_stops_v, int) and stored_stops_v >= cur_stops_v:
                stops_val = _validate_color_stops(stored.get("color_stops"))
                if stops_val is not None:
                    cfg["color_stops"] = stops_val
            # else: keep defaults (already deep-copied above). _stops_v is
            # automatically refreshed when save_emo2_stats writes the file.
            # Pace multipliers — validate range and ignore garbage.
            for k in ("anim_pace_x10", "form_pace_x10"):
                v = stored.get(k)
                if isinstance(v, (int, float)) and 5 <= int(v) <= 50:
                    cfg[k] = int(v)
            # Per-layout config
            layouts = stored.get("layouts")
            if isinstance(layouts, dict):
                for lid, lc in layouts.items():
                    if not isinstance(lid, str) or not isinstance(lc, dict):
                        continue
                    cfg["layouts"].setdefault(lid, {})
                    # v3 native: text_source + text_format
                    ts = lc.get("text_source")
                    if isinstance(ts, str) and ts in TEXT_SOURCES:
                        cfg["layouts"][lid]["text_source"] = ts
                    tf = lc.get("text_format")
                    if isinstance(tf, str) and tf in TEXT_FORMATS:
                        cfg["layouts"][lid]["text_format"] = tf
                    # v2 fallback: text_mode (only used if v3 fields missing)
                    tm = lc.get("text_mode")
                    if (isinstance(tm, str) and tm in TEXT_MODES
                            and "text_source" not in lc
                            and "text_format" not in lc):
                        _apply_legacy_text_mode(cfg["layouts"][lid], tm)
            # v1 → v3 migration: top-level text_mode applied to active layout.
            legacy_tm = stored.get("text_mode")
            if isinstance(legacy_tm, str) and legacy_tm in TEXT_MODES:
                cfg["layouts"].setdefault(cfg["active"], {})
                _apply_legacy_text_mode(cfg["layouts"][cfg["active"]], legacy_tm)
                log(f"emo2-stats: migrated v1 text_mode={legacy_tm} → "
                    f"layouts[{cfg['active']}].(text_source,text_format)")
        except Exception as e:
            log(f"emo2-stats config read failed, using defaults: {e}")
    return cfg


def emo2_stats_text_source_for(cfg: dict, layout_id: str) -> str:
    """Per-layout text source lookup with safe fallback to 'both'."""
    layouts = cfg.get("layouts") or {}
    entry = layouts.get(layout_id) or {}
    ts = entry.get("text_source")
    if isinstance(ts, str) and ts in TEXT_SOURCES:
        return ts
    return "both"


def emo2_stats_text_format_for(cfg: dict, layout_id: str) -> str:
    """Per-layout text format lookup with safe fallback to 'pct_reset'."""
    layouts = cfg.get("layouts") or {}
    entry = layouts.get(layout_id) or {}
    tf = entry.get("text_format")
    if isinstance(tf, str) and tf in TEXT_FORMATS:
        return tf
    return "pct_reset"


def emo2_stats_text_placement_for(cfg: dict, layout_id: str) -> str:
    """Per-layout text placement lookup. Default 'middle' matches legacy positions."""
    layouts = cfg.get("layouts") or {}
    entry = layouts.get(layout_id) or {}
    tp = entry.get("text_placement")
    if isinstance(tp, str) and tp in TEXT_PLACEMENTS:
        return tp
    return "middle"


def _enforce_layout_invariants(cfg: dict) -> None:
    """Mutate cfg in place to satisfy per-layout constraints.

    Item 4:   per-layout text_placement allowed-sets (LAYOUT_PLACEMENT_ALLOWED).
              If the persisted value isn't in the allowed set, pick the first
              allowed entry — or fall through (no placement) for empty sets.

    NOTE: the old corner_chip + pct_reset clamp was removed (item 4.2):
    chip now renders pct as a partial bottom-up border fill (chip_fill_l/r)
    while the chip body shows just the reset-time text. Combination valid.
    """
    layouts = cfg.setdefault("layouts", {})
    for lid, entry in layouts.items():
        if not isinstance(entry, dict):
            continue
        # 4.x placement matrix
        allowed = LAYOUT_PLACEMENT_ALLOWED.get(lid)
        if allowed is None:
            # Unknown layout id (gallery-only / future). Leave placement alone.
            continue
        cur = entry.get("text_placement")
        if not isinstance(cur, str) or cur not in TEXT_PLACEMENTS:
            cur = None
        if allowed:
            if cur not in allowed:
                entry["text_placement"] = allowed[0]
        else:
            # Empty set → strip placement key (layout has its own scheme).
            entry.pop("text_placement", None)


# Legacy text-mode lookup retained as a stub returning the most-similar
# v2 value for downstream code that still uses it. Prefer the source/format
# pair above when adding new code.
def emo2_stats_text_mode_for(cfg: dict, layout_id: str) -> str:
    src = emo2_stats_text_source_for(cfg, layout_id)
    fmt = emo2_stats_text_format_for(cfg, layout_id)
    if src == "off":
        return "none"
    if fmt == "pct":
        return "pct"
    if fmt == "reset":
        return "reset"
    return "both"


def save_emo2_stats(cfg: dict) -> None:
    try:
        EMO2_STATS_FILE.parent.mkdir(parents=True, exist_ok=True)
        EMO2_STATS_FILE.write_text(json.dumps(cfg, indent=2))
    except OSError as e:
        log(f"Could not persist emo2-stats config: {e}")


async def _handle_orientation(request: web.Request) -> web.Response:
    """POST /api/orientation  body: {"value": "vertical" | "horizontal"}.
    Sends the CTRL byte; ESP persists to NVS. Daemon doesn't track state
    (no replay on reconnect — the firmware's own NVS does that)."""
    state: DaemonState = request.app["state"]
    try:
        body = await request.json()
    except Exception:
        return web.json_response({"error": "invalid JSON"}, status=400)
    val = body.get("value")
    cmd = ROTATION_CTRL.get(val)
    if cmd is None:
        return web.json_response(
            {"error": f"value must be one of {list(ROTATION_CTRL.keys())}"}, status=400)
    # Record the pick host-side (item 10) so a web reload restores the right
    # aspect + placement-allowed set, then push the CTRL byte if connected.
    state.orientation = val
    save_orient(val)
    await _broadcast_ws(state, {"type": "orient", "value": val})
    delivered = state.ble_connected
    if delivered:
        await state.ctrl_queue.put(cmd)
    log(f"orientation: {val} → CTRL 0x{cmd:02X}" + ("" if delivered else " (not delivered — BLE offline)"))
    return web.json_response({"ok": True, "value": val, "delivered": delivered})


async def _handle_emo2_stats_config(request: web.Request) -> web.Response:
    """GET → current %-layout picks. POST → {default, extras} validated + persisted."""
    state: DaemonState = request.app["state"]

    if request.method == "GET":
        return web.json_response({
            "config": state.emo2_stats,
            "layouts": list(EMO2_STATS_LAYOUTS),
            "orient": state.orientation,   # item 10 — let the web init the right aspect
        })

    try:
        body = await request.json()
    except Exception:
        return web.json_response({"error": "invalid JSON"}, status=400)

    new_cfg = dict(state.emo2_stats)
    d = body.get("default")
    if d is not None:
        if not (isinstance(d, str) and d in EMO2_STATS_LAYOUTS):
            return web.json_response(
                {"error": f"default must be one of {list(EMO2_STATS_LAYOUTS)}"},
                status=400)
        new_cfg["default"] = d

    ex = body.get("extras")
    if ex is not None:
        if not isinstance(ex, list) or not all(isinstance(x, str) for x in ex):
            return web.json_response({"error": "extras must be a list of strings"}, status=400)
        seen = set()
        clean = []
        for x in ex:
            if x in EMO2_STATS_LAYOUTS and x != new_cfg["default"] and x not in seen:
                seen.add(x); clean.append(x)
        new_cfg["extras"] = clean

    active = body.get("active")
    active_changed = False
    if active is not None:
        if not (isinstance(active, str) and active in EMO2_STATS_LAYOUTS):
            return web.json_response(
                {"error": f"active must be one of {list(EMO2_STATS_LAYOUTS)}"},
                status=400)
        if active != new_cfg.get("active"):
            active_changed = True
        new_cfg["active"] = active
    # Keep active consistent with default if default OR extras changed AND
    # active no longer makes sense. Only run this fixup when the request
    # actually changed the curated set — running it on every POST (incl.
    # text-mode-only patches) was silently switching active to default if
    # the user's persisted active wasn't in the current extras list. That
    # caused the "text-mode click → layout snaps to chip" bug.
    if active is None and (d is not None or ex is not None):
        if new_cfg.get("active") not in ([new_cfg["default"]] + new_cfg.get("extras", [])):
            new_cfg["active"] = new_cfg["default"]
            active_changed = True

    # Per-layout text patches: {layouts: {<id>: {text_source, text_format}}}
    # OR shorthands {text_source: ...} / {text_format: ...} for active.
    # Legacy {text_mode: ...} still accepted — mapped via _LEGACY_TEXT_MODE_MAP.
    layouts_patch = body.get("layouts")
    if isinstance(layouts_patch, dict):
        new_cfg.setdefault("layouts", {})
        for lid, lc in layouts_patch.items():
            if not (isinstance(lid, str) and lid in EMO2_STATS_LAYOUTS
                    and isinstance(lc, dict)):
                continue
            new_cfg["layouts"].setdefault(lid, {})
            ts = lc.get("text_source")
            if isinstance(ts, str) and ts in TEXT_SOURCES:
                new_cfg["layouts"][lid]["text_source"] = ts
            tf = lc.get("text_format")
            if isinstance(tf, str) and tf in TEXT_FORMATS:
                new_cfg["layouts"][lid]["text_format"] = tf
            tp = lc.get("text_placement")
            if isinstance(tp, str) and tp in TEXT_PLACEMENTS:
                new_cfg["layouts"][lid]["text_placement"] = tp
            tm = lc.get("text_mode")
            if isinstance(tm, str) and tm in TEXT_MODES:
                _apply_legacy_text_mode(new_cfg["layouts"][lid], tm)

    text_patch_seen = bool(layouts_patch)
    shorthand_src = body.get("text_source")
    if isinstance(shorthand_src, str) and shorthand_src in TEXT_SOURCES:
        new_cfg.setdefault("layouts", {})
        new_cfg["layouts"].setdefault(new_cfg["active"], {})
        new_cfg["layouts"][new_cfg["active"]]["text_source"] = shorthand_src
        text_patch_seen = True
    shorthand_fmt = body.get("text_format")
    if isinstance(shorthand_fmt, str) and shorthand_fmt in TEXT_FORMATS:
        new_cfg.setdefault("layouts", {})
        new_cfg["layouts"].setdefault(new_cfg["active"], {})
        new_cfg["layouts"][new_cfg["active"]]["text_format"] = shorthand_fmt
        text_patch_seen = True
    shorthand_place = body.get("text_placement")
    if isinstance(shorthand_place, str) and shorthand_place in TEXT_PLACEMENTS:
        new_cfg.setdefault("layouts", {})
        new_cfg["layouts"].setdefault(new_cfg["active"], {})
        new_cfg["layouts"][new_cfg["active"]]["text_placement"] = shorthand_place
        text_patch_seen = True
    shorthand_tm = body.get("text_mode")
    if isinstance(shorthand_tm, str) and shorthand_tm in TEXT_MODES:
        new_cfg.setdefault("layouts", {})
        new_cfg["layouts"].setdefault(new_cfg["active"], {})
        _apply_legacy_text_mode(new_cfg["layouts"][new_cfg["active"]], shorthand_tm)
        text_patch_seen = True

    # Top-level: layout colour + clock style.
    layout_color_changed = False
    lc_val = body.get("layout_color")
    if isinstance(lc_val, str):
        if not is_color_value(lc_val):
            return web.json_response(
                {"error": f"layout_color must be one of {list(LAYOUT_COLORS)} or '#RRGGBB'"},
                status=400)
        if new_cfg.get("layout_color") != lc_val:
            layout_color_changed = True
        new_cfg["layout_color"] = lc_val
    clock_style_changed = False
    cs_val = body.get("clock_style")
    if isinstance(cs_val, str):
        # Transparently rewrite legacy v1 names (minimal/big/dot) to their
        # closest v2 equivalents — keeps old browser tabs / scripts working.
        cs_val = _CLOCK_STYLE_LEGACY_MAP.get(cs_val, cs_val)
        if cs_val not in CLOCK_STYLES:
            return web.json_response(
                {"error": f"clock_style must be one of {list(CLOCK_STYLES)}"}, status=400)
        if new_cfg.get("clock_style") != cs_val:
            clock_style_changed = True
        new_cfg["clock_style"] = cs_val

    # Clock colour — three modes: "default" (per-style hardcoded colour),
    # "auto" (follow the live %-gradient) or a "#RRGGBB" custom hex.
    # Rejected values fall through with 400; absent value leaves state unchanged.
    if "clock_color" in body:
        cc_val = body.get("clock_color")
        if not (isinstance(cc_val, str)
                and (cc_val in ("default", "auto") or HEX_COLOR_RE.match(cc_val))):
            return web.json_response(
                {"error": "clock_color must be 'default', 'auto' or '#RRGGBB'"},
                status=400)
        if new_cfg.get("clock_color") != cc_val:
            clock_style_changed = True   # treat as style change → triggers push
        new_cfg["clock_color"] = cc_val
    # Pace multipliers — ints in range 5..50 (×10 encoding: 5 = 0.5×, 50 = 5×).
    pace_changed = False
    for k in ("anim_pace_x10", "form_pace_x10"):
        if k in body:
            v = body.get(k)
            if not isinstance(v, (int, float)) or int(v) < 5 or int(v) > 50:
                return web.json_response(
                    {"error": f"{k} must be an integer in [5..50]"}, status=400)
            new_val = int(v)
            if new_cfg.get(k) != new_val:
                pace_changed = True
            new_cfg[k] = new_val

    # Colour-by-% gradient stops.
    color_stops_changed = False
    if "color_stops" in body:
        stops_norm = _validate_color_stops(body.get("color_stops"))
        if stops_norm is None:
            return web.json_response(
                {"error": "color_stops must be a list of 2..4 {pct, color} entries with #RRGGBB"},
                status=400)
        if new_cfg.get("color_stops") != stops_norm:
            color_stops_changed = True
        new_cfg["color_stops"] = stops_norm

    # Gradient interpolation mode — "step" or "smooth". Defaults preserved
    # when the field is absent. Invalid values are rejected with 400 instead
    # of silently coerced, so the web UI can surface the error.
    if "gradient_mode" in body:
        gm = body.get("gradient_mode")
        if gm not in ("step", "smooth"):
            return web.json_response(
                {"error": "gradient_mode must be 'step' or 'smooth'"},
                status=400)
        if new_cfg.get("gradient_mode") != gm:
            color_stops_changed = True   # treat as colour change → triggers full push
        new_cfg["gradient_mode"] = gm

    # Per-layout sanity clamps (item 5.1 + per-layout placement matrix).
    # Server is the single source of truth: even if the web UI somehow ships
    # a forbidden combination (stale browser tab, scripted POST, …) we sanitise
    # here so firmware never sees an invalid state.
    _enforce_layout_invariants(new_cfg)

    state.emo2_stats = new_cfg
    save_emo2_stats(new_cfg)

    # Stage 3 / Phase L1 — Phase D backward bridge removed. Web POSTs
    # per-state changes straight to /api/emo2-config now (since Phase D
    # Stage 2 lifted layout/text/layout_color UI into per-state tabs).
    # The bridge mirrored global picks into ALL four per-state entries,
    # which was needed only while the UI still wrote to the global
    # endpoint. Keeping it around now would silently overwrite a user's
    # per-state choices any time a curl script or stale browser tab hit
    # the stats endpoint — net negative.
    active_src   = emo2_stats_text_source_for(new_cfg, new_cfg["active"])
    active_fmt   = emo2_stats_text_format_for(new_cfg, new_cfg["active"])
    active_place = emo2_stats_text_placement_for(new_cfg, new_cfg["active"])
    active_tm    = emo2_stats_text_mode_for  (new_cfg, new_cfg["active"])
    log(f"emo2-stats: default={new_cfg['default']} active={new_cfg['active']} "
        f"text[{new_cfg['active']}]=({active_src},{active_fmt},{active_place}) "
        f"layout_color={new_cfg.get('layout_color','auto')} "
        f"clock_style={new_cfg.get('clock_style','off')}")

    # Phase B: push the whole config blob so ESP picks up the new active
    # layout + text mode + (unchanged) per-state form/op/color in one shot.
    # Debounced (~400 ms) so a rapid gradient-slider drag doesn't fire one
    # full JSON push per POST — that storms NVS writes and reboots the ESP.
    # Immediate-feedback CTRL bytes (layout/text/colour/clock) are still
    # sent right away — they're one setter call each on firmware, cheap.
    if state.ble_connected and state.session is not None:
        _schedule_push_emo2(state, delay=0.4)
    if state.ble_connected:
        # Helper — also fires the new dedicated text-source / text-format
        # CTRL bytes (0x51-0x57) alongside the legacy text_mode (0x44-0x47).
        # Legacy mode collapses (source, format) into a 4-value enum that
        # loses the session/weekly distinction (e.g. session+pct → 'pct' →
        # source=BOTH on firmware). Sending the new bytes preserves intent.
        async def _push_text_ctrls():
            tcmd = TEXT_MODE_CTRL.get(active_tm)
            if tcmd is not None:
                await state.ctrl_queue.put(tcmd)
            scmd2 = TEXT_SOURCE_CTRL.get(active_src)
            if scmd2 is not None:
                await state.ctrl_queue.put(scmd2)
            fcmd2 = TEXT_FORMAT_CTRL.get(active_fmt)
            if fcmd2 is not None:
                await state.ctrl_queue.put(fcmd2)
            pcmd2 = TEXT_PLACEMENT_CTRL.get(active_place)
            if pcmd2 is not None:
                await state.ctrl_queue.put(pcmd2)

        if active_changed:
            scmd = STATS_LAYOUT_CTRL.get(new_cfg["active"])
            if scmd is not None:
                await state.ctrl_queue.put(scmd)
            await _push_text_ctrls()
        elif text_patch_seen:
            await _push_text_ctrls()
        if layout_color_changed:
            lcmd = LAYOUT_COLOR_CTRL.get(new_cfg["layout_color"])
            if lcmd is not None:
                await state.ctrl_queue.put(lcmd)
        if clock_style_changed:
            ccmd = CLOCK_STYLE_CTRL.get(new_cfg["clock_style"])
            if ccmd is not None:
                await state.ctrl_queue.put(ccmd)

    return web.json_response({"ok": True, "config": new_cfg})


async def _handle_emo2_test_pct(request: web.Request) -> web.Response:
    """Push a forced pct value to the firmware for live colour preview.

    Web sends `{pct: 0..100}` when the user moves the gradient-editor TEST
    slider. Daemon ships it via the RX channel as a minimal cfg blob —
    `{"cfg":{"test_pct":N}}`. Firmware applies the override to the colour
    pipeline for TEST_PCT_TTL_MS (~6 s) and then snaps back to real data.
    No on-disk state — purely transient.
    """
    state: DaemonState = request.app["state"]
    try:
        body = await request.json()
    except Exception:
        return web.json_response({"error": "invalid JSON"}, status=400)
    pct = body.get("pct")
    if not isinstance(pct, (int, float)) or pct < -1 or pct > 100:
        return web.json_response({"error": "pct must be -1..100"}, status=400)
    if not state.ble_connected or state.session is None:
        # Device offline — just ack so the slider doesn't error out.
        return web.json_response({"ok": True, "queued": False})
    blob = {"cfg": {"test_pct": float(pct)}}
    try:
        ok = await state.session.write_payload(blob)
    except Exception as e:
        return web.json_response({"error": f"BLE write failed: {e}"}, status=503)
    return web.json_response({"ok": True, "queued": bool(ok)})


def compute_emo2_state(state: "DaemonState") -> str:
    """Map current daemon facts to one of the 4 emo2 states."""
    if state.auth_error:
        return "token_expired"
    if not state.ble_connected:
        return "ble_off"
    if state.last_payload is None:
        return "connecting"
    return "connected"


def _form_ctrl(name: str) -> Optional[int]:
    if name in FORM_NAMES:
        return 0x80 + FORM_NAMES.index(name)
    return None


def _op_ctrl(name: str) -> Optional[int]:
    if name in OP_NAMES:
        return 0xA0 + 1 + OP_NAMES.index(name)
    return None


def _form_name_idx(name: str) -> int:
    return FORM_NAMES.index(name) if name in FORM_NAMES else -1


def _op_name_idx(name: str) -> int:
    return OP_NAMES.index(name) if name in OP_NAMES else -1


def build_emo2_cfg_blob(state: "DaemonState") -> dict:
    """Assemble the single JSON config blob the ESP-side state machine
    consumes. Daemon converts string names → name-list indices to keep
    the payload small (fits the 512-byte RX buffer)."""
    states_out = {}
    for sid, sc in (state.emo2_config or {}).items():
        if not isinstance(sc, dict):
            continue
        forms = [_form_name_idx(n) for n in (sc.get("forms") or [])]
        ops   = [_op_name_idx(n)   for n in (sc.get("ops")   or [])]
        # Phase D: ship the full per-state visual stack to firmware. New
        # keys are OPTIONAL — old firmware ignores unknown keys, so this
        # is safe to send unconditionally.
        out = {
            "forms": [i for i in forms if i >= 0],
            "ops":   [i for i in ops   if i >= 0],
            "color": sc.get("color", "auto"),
        }
        # Layout is GLOBAL now (layouts.active below) — no per-state layout.
        if "text_source" in sc:    out["text_source"]    = sc["text_source"]
        if "text_format" in sc:    out["text_format"]    = sc["text_format"]
        if "text_placement" in sc: out["text_placement"] = sc["text_placement"]
        if "layout_color" in sc:   out["layout_color"]   = sc["layout_color"]
        states_out[sid] = out
    stats = state.emo2_stats or {}
    # Build a derived text_mode-per-layout dict from the new source+format pair
    # so firmware that only understands the legacy text_mode keeps working.
    text_map = {}
    for lid in (stats.get("layouts") or {}):
        text_map[lid] = emo2_stats_text_mode_for(stats, lid)
    # Active-layout placement — single value, applied by firmware to the
    # currently-shown layout. The CTRL byte covers the "instant" path; this
    # JSON field is the durable initial-state seed on re-connect.
    active_lid = stats.get("active", "bezel_orbit")
    out_cfg = {
        "states":  states_out,
        "layouts": {
            "active":         active_lid,
            "text":           text_map,
            # New native fields for the ACTIVE layout. The legacy text_mode
            # in `text` is lossy (e.g. src=SESSION+fmt=PCT collapses to
            # "pct" → src=BOTH+fmt=PCT on restore), so settings didn't
            # survive a reboot. These two keys preserve the user's pick
            # verbatim.
            "text_source":    emo2_stats_text_source_for   (stats, active_lid),
            "text_format":    emo2_stats_text_format_for   (stats, active_lid),
            "text_placement": emo2_stats_text_placement_for(stats, active_lid),
        },
    }
    # Layout-fill colour override (cyan/amber/red/auto). Without this in the
    # initial cfg blob, ESP boots with NVS-restored value which can diverge
    # from the daemon-stored setting (e.g. user edited via web while ESP was
    # offline). Pushing on every JSON sync keeps them in lockstep.
    lc = stats.get("layout_color")
    # layout_color may be a built-in keyword OR a "#RRGGBB" custom hex.
    if isinstance(lc, str) and is_color_value(lc):
        out_cfg["layout_color"] = lc
    # Clock style — same reasoning as layout_color.
    cs = stats.get("clock_style")
    if isinstance(cs, str) and cs in CLOCK_STYLES:
        out_cfg["clock_style"] = cs
    # Clock colour — global override with THREE modes: "default" (per-style
    # hardcoded colour → 0xFF), "auto" (follow the %-gradient → 0xA0) or a
    # "#RRGGBB" custom hex (→ 0x80). ALWAYS sent so firmware can reset the
    # override when the user switches between modes. Note: clock accepts
    # "default" (layout_color does not), so it can't reuse is_color_value().
    cc = stats.get("clock_color")
    if isinstance(cc, str) and (cc in ("default", "auto") or HEX_COLOR_RE.match(cc)):
        out_cfg["clock_color"] = cc
    # Colour-by-% stops piggyback the cfg JSON push — firmware parses + saves.
    stops = stats.get("color_stops")
    if isinstance(stops, list) and 2 <= len(stops) <= 4:
        out_cfg["color_stops"] = [
            {"pct": s["pct"], "color": s["color"]}
            for s in stops
            if isinstance(s, dict) and "pct" in s and "color" in s
        ]
    # Gradient interpolation mode — companion to color_stops. Only sent when
    # explicitly set so old firmware (which doesn't parse this key) sees its
    # existing step behaviour as before. New firmware reads "step"/"smooth"
    # from cfg JSON and routes pct_color_stepped() vs pct_color_smooth().
    gm = stats.get("gradient_mode")
    if gm in ("step", "smooth"):
        out_cfg["gradient_mode"] = gm
    # Pace multipliers — firmware reads as integer ×10.
    for k in ("anim_pace_x10", "form_pace_x10"):
        v = stats.get(k)
        if isinstance(v, (int, float)) and 5 <= int(v) <= 50:
            out_cfg[k] = int(v)
    return {"cfg": out_cfg}


# Debounce gate for push_emo2_full_config. Without this, a gradient-slider
# drag fires 5-6 POSTs / 2s, each pushing the full JSON config blob to the
# ESP — that triggers a parse + multiple flash writes per push and was
# causing watchdog reboots. Coalesces bursts into a single push.
_push_emo2_task: "asyncio.Task | None" = None

def _schedule_push_emo2(state: "DaemonState", delay: float = 0.4) -> None:
    """Cancel any pending push and schedule a fresh one to fire after `delay`
    seconds of quiet. Fast-path direct calls (e.g. on reconnect) still use
    push_emo2_full_config — only the rapid-edit path goes through here."""
    global _push_emo2_task
    if _push_emo2_task is not None and not _push_emo2_task.done():
        _push_emo2_task.cancel()
    async def _fire():
        try:
            await asyncio.sleep(delay)
            if state.ble_connected and state.session is not None:
                await push_emo2_full_config(state.session, state)
        except asyncio.CancelledError:
            pass
    _push_emo2_task = asyncio.create_task(_fire())


async def push_emo2_full_config(session: "Session", state: "DaemonState") -> None:
    """Send the whole per-state + layout config as one JSON RX-write. ESP
    parses, applies, and persists to NVS — daemon no longer drives form/op
    rotation tick-by-tick (Phase A moved that to firmware).

    Also re-sends layout_color + clock_style CTRL bytes — these aren't in the
    JSON blob (firmware doesn't parse them out of cfg yet), so on every
    reconnect we replay them via the simple CTRL channel."""
    if session is None or not state.ble_connected:
        return
    try:
        blob = build_emo2_cfg_blob(state)
        ok = await session.write_payload(blob)
        if not ok:
            log("emo2 cfg push failed (write_payload returned false)")
    except Exception as e:
        log(f"emo2 cfg push exception: {e}")
    # Replay the per-display knobs via CTRL bytes. Old firmware will ignore
    # any byte it doesn't recognise (default branch in CTRL dispatch).
    stats = state.emo2_stats or {}
    lc_val = stats.get("layout_color", "auto")
    lcmd = LAYOUT_COLOR_CTRL.get(lc_val)
    if lcmd is not None:
        await state.ctrl_queue.put(lcmd)
    cs_val = stats.get("clock_style", "off")
    ccmd = CLOCK_STYLE_CTRL.get(cs_val)
    if ccmd is not None:
        await state.ctrl_queue.put(ccmd)


def emo2_state_changed_ws(state: "DaemonState") -> None:
    """Compute current state + broadcast to web if it transitioned.
    Replaces the WS-broadcast that used to live inside push_emo2_visuals."""
    cur = compute_emo2_state(state)
    if cur != state.emo2_state:
        state.emo2_state = cur
        # Fire-and-forget on the event loop so synchronous callers (like
        # poll_api transitions) don't await.
        try:
            loop = asyncio.get_event_loop()
            loop.create_task(_broadcast_ws(state, {"type": "emo2_state", "state": cur}))
        except RuntimeError:
            pass


def save_lang(lang: str) -> None:
    try:
        LANG_FILE.parent.mkdir(parents=True, exist_ok=True)
        LANG_FILE.write_text(lang)
    except OSError as e:
        log(f"Could not persist language: {e}")


def save_orient(value: str) -> None:
    try:
        ORIENT_FILE.parent.mkdir(parents=True, exist_ok=True)
        ORIENT_FILE.write_text(value)
    except OSError as e:
        log(f"Could not persist orientation: {e}")


async def _handle_lang(request: web.Request) -> web.Response:
    """POST /api/lang  body: {"lang": "en"|"ru"}  → persist + CTRL set byte."""
    state: DaemonState = request.app["state"]
    try:
        body = await request.json()
    except Exception:
        return web.json_response({"error": "invalid JSON"}, status=400)

    lang = str(body.get("lang", "")).lower()
    if lang not in LANG_SET_MAP:
        return web.json_response({"error": "lang must be 'en' or 'ru'"}, status=400)

    state.firmware_lang = lang
    save_lang(lang)
    if state.ble_connected:
        await state.ctrl_queue.put(LANG_SET_MAP[lang])
    await _broadcast_ws(state, {"type": "lang", "lang": lang})
    return web.json_response({"ok": True, "lang": lang})


async def _handle_ws(request: web.Request) -> web.WebSocketResponse:
    state: DaemonState = request.app["state"]
    ws = web.WebSocketResponse()
    await ws.prepare(request)

    state.ws_clients.add(ws)
    # Send current state on connect
    if state.last_payload:
        await ws.send_str(json.dumps({"type": "data", "payload": state.last_payload}))
    await ws.send_str(json.dumps({
        "type": "ble",
        "state": "connected" if state.ble_connected else "disconnected",
        "address": state.ble_address,
    }))
    await ws.send_str(json.dumps({"type": "lang", "lang": state.firmware_lang}))
    await ws.send_str(json.dumps({"type": "orient", "value": state.orientation}))
    await ws.send_str(json.dumps({"type": "auth", "ok": not state.auth_error}))
    await ws.send_str(json.dumps({"type": "emo2_state", "state": state.emo2_state}))

    try:
        async for msg in ws:
            if msg.type == 1:  # TEXT
                try:
                    data = json.loads(msg.data)
                    action = data.get("action", "")
                    if action == "screen":
                        cmd = SCREEN_MAP.get(data.get("screen", "").lower())
                        if cmd:
                            await state.ctrl_queue.put(cmd)
                    elif action == "refresh":
                        await state.ctrl_queue.put(CTRL_REFRESH)
                    elif action in ACTION_MAP:
                        _cmd = ACTION_MAP[action]
                        log(f"ACTION: '{action}' (CTRL 0x{_cmd:02X}) requested via WS")
                        await state.ctrl_queue.put(_cmd)
                except json.JSONDecodeError:
                    pass
    finally:
        state.ws_clients.discard(ws)

    return ws


@web.middleware
async def no_cache_html_middleware(request: web.Request, handler):
    """Send `Cache-Control: no-store` on HTML responses so that iterating on
    the web UI doesn't get blocked by aggressive browser caching. Affects
    `/` (dashboard) and any `/static/*.html` gallery. Static JS/CSS/images
    still cache normally — those rarely change."""
    response = await handler(request)
    path = request.path
    if path == "/" or path.endswith(".html"):
        response.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
        response.headers["Pragma"] = "no-cache"
        response.headers["Expires"] = "0"
        # aiohttp's FileResponse auto-adds ETag + Last-Modified, which let
        # browsers fire conditional GET (If-None-Match / If-Modified-Since) →
        # the server returns 304 and the browser renders its stale cached
        # body. Strip both validators so every HTML hit is a fresh 200.
        response.headers.pop("ETag", None)
        response.headers.pop("Last-Modified", None)
    return response


def create_http_app(state: DaemonState) -> web.Application:
    app = web.Application(middlewares=[no_cache_html_middleware])
    app["state"] = state

    app.router.add_get("/", _handle_index)
    app.router.add_get("/api/status", _handle_status)
    app.router.add_post("/api/screen", _handle_screen)
    app.router.add_post("/api/refresh", _handle_refresh)
    app.router.add_post("/api/action", _handle_action)
    app.router.add_post("/api/halo-live", _handle_halo_live)
    app.router.add_post  ("/api/oauth-token", _handle_oauth_token)
    app.router.add_delete("/api/oauth-token", _handle_oauth_token)
    app.router.add_get ("/api/emo2-config", _handle_emo2_config)
    app.router.add_post("/api/emo2-config", _handle_emo2_config)
    app.router.add_get ("/api/emo2-stats-config", _handle_emo2_stats_config)
    app.router.add_post("/api/emo2-stats-config", _handle_emo2_stats_config)
    app.router.add_post("/api/emo2-test-pct",     _handle_emo2_test_pct)
    app.router.add_post("/api/orientation",       _handle_orientation)
    app.router.add_post("/api/ota/upload",        _handle_ota_upload)
    app.router.add_post("/api/lang", _handle_lang)
    app.router.add_get("/api/ws", _handle_ws)
    # Diagnostic endpoint — returns BLE reconnect counters + tail of daemon.log.
    # Curl-friendly for support ("send me curl http://localhost:8765/api/diag").
    app.router.add_get("/api/diag", _handle_diag)

    # Static files (CSS, JS, images)
    if WEB_DIR.exists():
        app.router.add_static("/static", WEB_DIR)

    return app


# ---------------------------------------------------------------------------
# CLI mode — connect to running daemon via HTTP
# ---------------------------------------------------------------------------

async def cli_mode(args: argparse.Namespace) -> None:
    port = args.port
    base = f"http://localhost:{port}"

    async with httpx.AsyncClient(timeout=5.0) as http:
        if args.status:
            # Graceful fallback: any kind of connection-level failure means
            # the daemon is not running. Don't dump a Python traceback —
            # exit cleanly with code 1 so shell wrappers can detect it.
            try:
                resp = await http.get(f"{base}/api/status", timeout=2.0)
                data = resp.json()
                print(f"BLE: {'connected' if data.get('ble_connected') else 'disconnected'}")
                if data.get("ble_address"):
                    print(f"Address: {data['ble_address']}")
                if data.get("last_data"):
                    d = data["last_data"]
                    print(f"Session: {d.get('s')}% (resets in {d.get('sr')}m)")
                    print(f"Weekly:  {d.get('w')}% (resets in {d.get('wr')}m)")
                    print(f"Status:  {d.get('st')}")
                print(f"Uptime: {data.get('uptime', 0)}s")
            except (httpx.ConnectError, httpx.TimeoutException,
                    httpx.ReadError, ConnectionRefusedError, OSError) as e:
                print(f"daemon: not running on port {port}", file=sys.stderr)
                sys.exit(1)

        elif args.screen:
            screen = args.screen.lower()
            if screen not in SCREEN_MAP:
                print(f"Unknown screen '{screen}'. Valid: {list(SCREEN_MAP.keys())}", file=sys.stderr)
                sys.exit(1)
            try:
                resp = await http.post(f"{base}/api/screen", json={"screen": screen})
                result = resp.json()
                if result.get("ok"):
                    print(f"Sent: screen → {screen}")
                else:
                    print(f"Error: {result.get('error', 'unknown')}", file=sys.stderr)
            except httpx.ConnectError:
                print(f"Daemon not running on port {port}", file=sys.stderr)
                sys.exit(1)

        elif args.refresh:
            try:
                resp = await http.post(f"{base}/api/refresh")
                result = resp.json()
                if result.get("ok"):
                    print("Sent: refresh")
                else:
                    print(f"Error: {result.get('error', 'unknown')}", file=sys.stderr)
            except httpx.ConnectError:
                print(f"Daemon not running on port {port}", file=sys.stderr)
                sys.exit(1)

        elif args.action:
            action = args.action.lower()
            if action not in ACTION_MAP:
                print(f"Unknown action '{action}'. Valid: {list(ACTION_MAP.keys())}", file=sys.stderr)
                sys.exit(1)
            try:
                resp = await http.post(f"{base}/api/action", json={"action": action})
                result = resp.json()
                if result.get("ok"):
                    print(f"Sent: action → {action}")
                else:
                    print(f"Error: {result.get('error', 'unknown')}", file=sys.stderr)
            except httpx.ConnectError:
                print(f"Daemon not running on port {port}", file=sys.stderr)
                sys.exit(1)

        elif args.ui:
            import subprocess as sp
            sp.run(["open", f"http://localhost:{port}"])


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

async def daemon_main(
    port: int = HTTP_PORT,
    lang: str = "en",
    external_stop: Optional[threading.Event] = None,
) -> None:
    """Run the daemon's main loop.

    `external_stop` lets a tray/GUI host that runs `daemon_main` from a
    background thread request a graceful shutdown — when the threading.Event
    fires, an asyncio task forwards it to the inner asyncio stop_event,
    triggering the same cleanup path as SIGTERM.
    """
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    # Signal handlers only work on the main thread. From a tray/GUI host the
    # daemon runs on a background thread, so we skip signals there — the
    # external_stop event covers shutdown instead.
    if threading.current_thread() is threading.main_thread():
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _stop)
            except (NotImplementedError, RuntimeError):
                # RuntimeError raised on Windows ProactorEventLoop and any
                # platform that doesn't support add_signal_handler. Fall
                # back to the synchronous signal API.
                try:
                    signal.signal(sig, _stop)
                except (ValueError, OSError):
                    pass

    # Forward an external (cross-thread) stop request into the asyncio loop.
    # Poll the threading.Event every 200 ms — light enough for a background
    # task and avoids needing call_soon_threadsafe plumbing on the caller side.
    async def _external_stop_watcher() -> None:
        while not stop_event.is_set():
            if external_stop is not None and external_stop.is_set():
                _stop()
                return
            await asyncio.sleep(0.2)

    if external_stop is not None:
        loop.create_task(_external_stop_watcher())

    # A web-chosen language (persisted) wins over the --lang default.
    if LANG_FILE.exists():
        saved = LANG_FILE.read_text().strip().lower()
        if saved in LANG_MAP:
            lang = saved
    state = DaemonState(firmware_lang=lang)
    # Restore the last web-chosen orientation (item 10) so the web reload shows
    # the correct aspect. Firmware NVS remains the real source of truth.
    if ORIENT_FILE.exists():
        saved_orient = ORIENT_FILE.read_text().strip().lower()
        if saved_orient in ("vertical", "horizontal"):
            state.orientation = saved_orient
    state.emo2_config = load_emo2_config()
    state.emo2_stats = load_emo2_stats()

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {state.poll_interval}s")
    log(f"HTTP port: {port}")

    # Hydrate CLAUDE_CODE_OAUTH_TOKEN from disk (set via web paste) before
    # building the first API client, so a daemon restart picks up the token
    # without requiring the user to re-export it.
    _load_persisted_oauth_token()
    state.api_client = _create_anthropic_client()
    if state.api_client is None:
        log("WARNING: No API credentials — will try again on first poll")

    # Start HTTP server
    app = create_http_app(state)
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "localhost", port)
    await site.start()
    log(f"HTTP server: http://localhost:{port}")

    backoff = 1
    try:
        while not stop_event.is_set():
            device = await scan_for_device()
            if device is None:
                log(f"Device not found, retrying in {backoff}s...")
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=backoff)
                except asyncio.TimeoutError:
                    pass
                backoff = min(backoff * 2, 60)
                continue

            # Re-create client if needed
            if state.api_client is None:
                state.api_client = _create_anthropic_client()
            if state.api_client is None:
                log("No API credentials, waiting...")
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=backoff)
                except asyncio.TimeoutError:
                    pass
                backoff = min(backoff * 2, 60)
                continue

            ok = await connect_and_run(device, stop_event, state)
            if not ok:
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=backoff)
                except asyncio.TimeoutError:
                    pass
                backoff = min(backoff * 2, 60)
            else:
                backoff = 1
    finally:
        try:
            await asyncio.wait_for(runner.cleanup(), timeout=3.0)
        except asyncio.TimeoutError:
            pass


def _run_tray_mode(port: int, lang: str) -> int:
    """Run daemon on a background thread + tray UI on the main thread.

    Returns process exit code. The tray UI library (rumps on macOS, pystray
    on Win/Linux) blocks the main thread until the user clicks Quit, at which
    point we signal the daemon thread and wait briefly for it to clean up.
    """
    # Import lazily so that --tray failures don't break CLI / daemon modes
    # on systems without rumps/pystray installed.
    try:
        from tray import run_tray
    except ImportError as exc:
        print(
            f"--tray requires the tray module + its deps "
            f"(rumps on macOS / pystray on Win/Linux): {exc}\n"
            f"Install: pip install rumps  (macOS)  OR  pip install pystray pillow",
            file=sys.stderr,
        )
        return 2

    stop_event = threading.Event()

    def _daemon_thread_target() -> None:
        try:
            asyncio.run(daemon_main(port=port, lang=lang, external_stop=stop_event))
        except Exception as exc:  # pragma: no cover  (defensive: never crash silently)
            print(f"Daemon thread crashed: {exc}", file=sys.stderr)

    daemon_thread = threading.Thread(
        target=_daemon_thread_target,
        name="ClauliDaemon",
        daemon=True,
    )
    daemon_thread.start()

    # Wait until the HTTP server actually accepts connections, so the tray
    # never shows a stale "disconnected" state before the daemon is ready.
    # 5 s cap — past that the daemon almost certainly failed to bind, and the
    # tray will surface the error via its status indicator instead.
    deadline = time.time() + 5.0
    while time.time() < deadline:
        try:
            r = httpx.get(f"http://localhost:{port}/api/status", timeout=0.3)
            if r.status_code == 200:
                break
        except Exception:
            time.sleep(0.1)

    # run_tray blocks until the user clicks Quit (which calls on_quit below).
    run_tray(
        port=port,
        on_quit=stop_event.set,
    )

    # Give the daemon thread up to 5 s to flush state + close BLE cleanly.
    daemon_thread.join(timeout=5.0)
    return 0


def main() -> None:
    parser = argparse.ArgumentParser(description="Claude Usage Tracker Daemon")
    parser.add_argument("--screen", metavar="NAME",
                        help="Switch ESP32 screen: usage, bt, splash, cycle, emo")
    parser.add_argument("--refresh", action="store_true",
                        help="Force data refresh on ESP32")
    parser.add_argument("--action", metavar="NAME",
                        help="Device action: reboot, view, anim")
    parser.add_argument("--status", action="store_true",
                        help="Show current daemon status")
    parser.add_argument("--ui", action="store_true",
                        help="Open Web UI in browser")
    parser.add_argument("--tray", action="store_true",
                        help="Run with menu-bar/system-tray UI (macOS / Win / Linux)")
    parser.add_argument("--lang", choices=["en", "ru"], default="en",
                        help="Firmware UI language (default: en)")
    parser.add_argument("--port", type=int, default=HTTP_PORT,
                        help=f"HTTP server port (default: {HTTP_PORT})")
    args = parser.parse_args()

    # CLI mode — talk to running daemon
    if args.screen or args.refresh or args.action or args.status or args.ui:
        asyncio.run(cli_mode(args))
        return

    # Tray mode — daemon in background thread + menu-bar UI on main thread
    if args.tray:
        sys.exit(_run_tray_mode(port=args.port, lang=args.lang))
        return

    # Daemon mode (foreground, no UI shell — used by daemon.sh)
    asyncio.run(daemon_main(port=args.port, lang=args.lang))


if __name__ == "__main__":
    main()
