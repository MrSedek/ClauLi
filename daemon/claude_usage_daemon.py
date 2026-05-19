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

# CTRL command bytes (must match firmware ble.cpp)
CTRL_SCREEN_USAGE = 0x01
CTRL_SCREEN_BT = 0x02
CTRL_SCREEN_SPLASH = 0x03
CTRL_SCREEN_CYCLE = 0x04
CTRL_SCREEN_EMO = 0x06
CTRL_REBOOT = 0x05         # reboot the ESP32
CTRL_CYCLE_VIEW = 0x07     # BOOT-click analog (cycle the current screen's view)
CTRL_NEXT_ANIM = 0x08      # advance emotion/animation now (skip the timer)
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
}

LANG_MAP = {"en": CTRL_LANG_EN, "ru": CTRL_LANG_RU}
LANG_SET_MAP = {"en": CTRL_LANG_EN_SET, "ru": CTRL_LANG_RU_SET}

ACTION_MAP = {
    "reboot": CTRL_REBOOT,
    "view": CTRL_CYCLE_VIEW,
    "anim": CTRL_NEXT_ANIM,
}

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0
HTTP_PORT = 8765

LANG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "lang"
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


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ---------------------------------------------------------------------------
# Daemon state — shared between BLE loop and HTTP server
# ---------------------------------------------------------------------------

@dataclass
class DaemonState:
    """Shared mutable state accessible from both BLE and HTTP contexts."""
    ble_connected: bool = False
    ble_address: Optional[str] = None
    last_payload: Optional[dict] = None
    last_poll_time: float = 0.0
    start_time: float = field(default_factory=time.time)
    poll_interval: int = POLL_INTERVAL
    firmware_lang: str = "en"
    ctrl_queue: asyncio.Queue = field(default_factory=asyncio.Queue)
    ws_clients: set = field(default_factory=set)
    api_client: Optional[Anthropic] = None
    auth_error: bool = False


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
    if _sync_keychain_to_sdk_credentials(force=force):
        try:
            provider = CredentialsFile()
            return Anthropic(credentials=provider)
        except Exception as e:
            log(f"SDK CredentialsFile setup failed: {e}")

    env_token = os.environ.get("CLAUDE_CODE_OAUTH_TOKEN")
    if env_token:
        log("Using CLAUDE_CODE_OAUTH_TOKEN from environment")
        return Anthropic(auth_token=env_token)

    log("No credentials available — API calls will fail")
    return None


# ---------------------------------------------------------------------------
# API polling
# ---------------------------------------------------------------------------

async def poll_api(client: Anthropic, state: DaemonState) -> Optional[dict]:
    try:
        token = await asyncio.to_thread(client._token_cache.get_token)
    except Exception as e:
        msg = str(e)
        if not state.auth_error:  # log the actionable hint once per outage
            if "invalid_grant" in msg:
                log("AUTH: refresh token invalid/expired. Re-authenticate in "
                    "the Claude Code app or run `claude` in a terminal — the "
                    "daemon re-reads the Keychain every poll and auto-recovers "
                    "once new credentials appear (no restart needed).")
            else:
                log(f"Token acquisition failed: {e}")
        state.auth_error = True
        return None
    if state.auth_error:
        state.auth_error = False
        log("AUTH: recovered — fresh credentials picked up")

    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"

    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None

    if resp.status_code == 401:
        log("API returned 401 — invalidating token cache")
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

    async def setup_refresh_subscription(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False

    async def write_ctrl(self, cmd: int) -> bool:
        """Send a control command byte to the ESP32."""
        try:
            await self.client.write_gatt_char(CTRL_CHAR_UUID, bytes([cmd]), response=False)
            log(f"CTRL: sent 0x{cmd:02X}")
            return True
        except BleakError as e:
            log(f"CTRL write failed: {e}")
            return False


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

    log("Connected")
    state.ble_connected = True
    state.ble_address = address
    await _broadcast_ws(state, {"type": "ble", "state": "connected", "address": address})

    session = Session(client, state)
    await session.setup_refresh_subscription()

    # Tell the firmware which UI language to render.
    await session.write_ctrl(LANG_MAP.get(state.firmware_lang, CTRL_LANG_EN))

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
            if session.refresh_requested.is_set() or elapsed >= state.poll_interval:
                session.refresh_requested.clear()
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

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        state.ble_connected = False
        try:
            await asyncio.wait_for(client.disconnect(), timeout=3.0)
        except (BleakError, asyncio.TimeoutError, Exception):
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
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

    await state.ctrl_queue.put(cmd)
    return web.json_response({"ok": True, "action": action, "cmd": cmd})


def save_lang(lang: str) -> None:
    try:
        LANG_FILE.parent.mkdir(parents=True, exist_ok=True)
        LANG_FILE.write_text(lang)
    except OSError as e:
        log(f"Could not persist language: {e}")


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
    await ws.send_str(json.dumps({"type": "auth", "ok": not state.auth_error}))

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
                        await state.ctrl_queue.put(ACTION_MAP[action])
                except json.JSONDecodeError:
                    pass
    finally:
        state.ws_clients.discard(ws)

    return ws


def create_http_app(state: DaemonState) -> web.Application:
    app = web.Application()
    app["state"] = state

    app.router.add_get("/", _handle_index)
    app.router.add_get("/api/status", _handle_status)
    app.router.add_post("/api/screen", _handle_screen)
    app.router.add_post("/api/refresh", _handle_refresh)
    app.router.add_post("/api/action", _handle_action)
    app.router.add_post("/api/lang", _handle_lang)
    app.router.add_get("/api/ws", _handle_ws)

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
            try:
                resp = await http.get(f"{base}/api/status")
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
            except httpx.ConnectError:
                print(f"Daemon not running on port {port}", file=sys.stderr)
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

async def daemon_main(port: int = HTTP_PORT, lang: str = "en") -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    # A web-chosen language (persisted) wins over the --lang default.
    if LANG_FILE.exists():
        saved = LANG_FILE.read_text().strip().lower()
        if saved in LANG_MAP:
            lang = saved
    state = DaemonState(firmware_lang=lang)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {state.poll_interval}s")
    log(f"HTTP port: {port}")

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
    parser.add_argument("--lang", choices=["en", "ru"], default="en",
                        help="Firmware UI language (default: en)")
    parser.add_argument("--port", type=int, default=HTTP_PORT,
                        help=f"HTTP server port (default: {HTTP_PORT})")
    args = parser.parse_args()

    # CLI mode — talk to running daemon
    if args.screen or args.refresh or args.action or args.status or args.ui:
        asyncio.run(cli_mode(args))
        return

    # Daemon mode
    asyncio.run(daemon_main(port=args.port, lang=args.lang))


if __name__ == "__main__":
    main()
