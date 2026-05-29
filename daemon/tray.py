"""Menu-bar / system-tray UI for ClauLi.

`run_tray(port, on_quit)` is the single public entry. It blocks the calling
thread until the user picks Quit, then calls `on_quit()` so the host process
can stop the daemon and exit. Two backends:

- macOS: `rumps` (native Cocoa NSStatusBar — templated icon adapts to
  dark/light, sub-menus, items + separators).
- Linux / Windows: `pystray` (PyQt-free abstraction over GTK/X11
  AppIndicator / Win32 Shell_NotifyIcon).

Both backends share the same menu shape:
    Open dashboard
    Refresh
    ─────
    Language ▸ English / Русский          (radio)
    Start at login                         (checkbox)
    ─────
    Status: …                              (read-only, live)
    Quit ClauLi

State (BLE-connected, current lang, autostart) is polled from the daemon
via http://localhost:{port}/api/status every 3 s. Posts go through the
same HTTP API the web UI uses, so the tray and web UI stay in sync.
"""
from __future__ import annotations

import json
import sys
import threading
import time
import urllib.error
import urllib.request
import webbrowser
from typing import Any, Callable, Dict, Optional

import autostart


# ---------------------------------------------------------------------------
# HTTP helpers (sync; tray runs on its own thread so blocking is fine)
# ---------------------------------------------------------------------------

def _http_get(url: str, timeout: float = 1.5) -> Optional[Dict[str, Any]]:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            if resp.status != 200:
                return None
            return json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, ConnectionError, TimeoutError, OSError, json.JSONDecodeError):
        return None


def _http_post(url: str, payload: Dict[str, Any], timeout: float = 2.0) -> bool:
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url, data=body, method="POST",
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return 200 <= resp.status < 300
    except (urllib.error.URLError, ConnectionError, TimeoutError, OSError):
        return False


# ---------------------------------------------------------------------------
# Icon — minimal templated PNG generated via PIL at import time. Keeps the
# repo free of binary assets the dev has to manually re-render on tweaks.
# ---------------------------------------------------------------------------

def _build_icon_bytes(size: int = 22, template: bool = True) -> bytes:
    """Return PNG bytes for the tray icon.

    Design: filled circle (eye) + small inner dot (pupil). Pure white on
    transparent so macOS templating tints it to whichever colour the menu
    bar background needs. pystray on Linux/Win uses the white-on-transparent
    image directly — looks fine on both dark and light panel themes.
    """
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        # If PIL isn't available we return a 1×1 transparent PNG so the
        # tray library still has *something* to anchor on; the user just
        # sees a blank slot. Install pillow to get a real icon.
        return (
            b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01"
            b"\x08\x06\x00\x00\x00\x1f\x15\xc4\x89\x00\x00\x00\rIDATx\x9cc\xf8\xcf"
            b"\xc0\x00\x00\x00\x03\x00\x01\x9eq\x10\xe1\x00\x00\x00\x00IEND\xaeB`\x82"
        )
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    s = size
    # Outer ring
    pad = max(2, s // 8)
    d.ellipse((pad, pad, s - pad - 1, s - pad - 1),
              outline=(255, 255, 255, 255), width=max(1, s // 11))
    # Inner pupil
    cx, cy = s / 2, s / 2
    r = s / 5
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(255, 255, 255, 255))
    # Save to bytes
    from io import BytesIO
    buf = BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def _build_icon_image():
    """Return a PIL.Image instance for pystray (which wants the live object)."""
    try:
        from PIL import Image
    except ImportError:
        return None
    from io import BytesIO
    return Image.open(BytesIO(_build_icon_bytes(size=64, template=False)))


# ---------------------------------------------------------------------------
# Shared state model — what the tray needs to render
# ---------------------------------------------------------------------------

class TrayState:
    def __init__(self, port: int):
        self.port = port
        self.connected: bool = False
        self.lang: str = "en"
        self.auth_error: bool = False
        self.last_seen: Optional[float] = None

    @property
    def dashboard_url(self) -> str:
        return f"http://localhost:{self.port}/"

    def refresh_from_daemon(self) -> bool:
        """Pull /api/status. Returns True if the daemon answered."""
        data = _http_get(f"http://localhost:{self.port}/api/status")
        if data is None:
            return False
        self.connected = bool(data.get("ble_connected"))
        self.lang = data.get("lang", "en") or "en"
        self.auth_error = bool(data.get("auth_error"))
        self.last_seen = time.time()
        return True

    def push_lang(self, lang: str) -> bool:
        ok = _http_post(
            f"http://localhost:{self.port}/api/lang", {"lang": lang}
        )
        if ok:
            self.lang = lang
        return ok

    def push_refresh(self) -> bool:
        return _http_post(f"http://localhost:{self.port}/api/refresh", {})


# ---------------------------------------------------------------------------
# Native dashboard window (macOS) — WKWebView in a plain NSWindow.
#
# rumps already spins up an NSApplication on the main thread, so we just
# attach a window to that existing runloop. This MUST be called on the main
# thread (it is — the rumps menu callback runs there). Returns the NSWindow
# so the caller can keep a strong reference (otherwise PyObjC GCs it and the
# window vanishes) and re-show it on the next click instead of spawning a
# fresh one each time.
# ---------------------------------------------------------------------------

def _make_native_macos_window(url: str, title: str = "ClauLi"):
    import AppKit
    import WebKit
    from Foundation import NSURL, NSURLRequest

    rect = AppKit.NSMakeRect(0, 0, 1180, 860)
    style = (
        AppKit.NSWindowStyleMaskTitled
        | AppKit.NSWindowStyleMaskClosable
        | AppKit.NSWindowStyleMaskMiniaturizable
        | AppKit.NSWindowStyleMaskResizable
    )
    win = AppKit.NSWindow.alloc().initWithContentRect_styleMask_backing_defer_(
        rect, style, AppKit.NSBackingStoreBuffered, False
    )
    win.setTitle_(title)
    # Keep the Python/ObjC object alive after the user closes the window so we
    # can re-open the same instance; without this the close button frees it
    # and the next makeKeyAndOrderFront_ touches a dangling pointer.
    win.setReleasedWhenClosed_(False)

    cfg = WebKit.WKWebViewConfiguration.alloc().init()
    webview = WebKit.WKWebView.alloc().initWithFrame_configuration_(rect, cfg)
    webview.setAutoresizingMask_(
        AppKit.NSViewWidthSizable | AppKit.NSViewHeightSizable
    )
    req = NSURLRequest.requestWithURL_(NSURL.URLWithString_(url))
    webview.loadRequest_(req)
    win.setContentView_(webview)
    win.center()
    return win


# ---------------------------------------------------------------------------
# macOS backend — rumps
# ---------------------------------------------------------------------------

def _run_tray_macos(port: int, on_quit: Callable[[], None]) -> None:
    import os
    import tempfile

    import rumps  # type: ignore

    state = TrayState(port=port)

    # Write icon PNG to a temp file — rumps expects a path, not bytes.
    icon_path = os.path.join(tempfile.gettempdir(), "clauli-tray.png")
    with open(icon_path, "wb") as f:
        f.write(_build_icon_bytes(size=44, template=True))

    class ClauliTray(rumps.App):
        def __init__(self):
            super().__init__(
                name="ClauLi",
                icon=icon_path,
                template=True,            # auto-tint to menu-bar text colour
                quit_button=None,         # we wire our own Quit so on_quit fires
            )
            # Strong ref to the native dashboard window so PyObjC doesn't GC
            # it between opens.
            self._win = None
            self.menu = [
                rumps.MenuItem("Open dashboard", callback=self._on_dashboard),
                rumps.MenuItem("Open in browser", callback=self._on_dashboard_browser),
                rumps.MenuItem("Refresh", callback=self._on_refresh),
                None,  # separator
                ("Language", [
                    rumps.MenuItem("English",  callback=lambda _: self._set_lang("en")),
                    rumps.MenuItem("Русский",  callback=lambda _: self._set_lang("ru")),
                ]),
                rumps.MenuItem("Start at login", callback=self._on_autostart),
                None,
                rumps.MenuItem("Status: …"),  # no callback → non-interactive
                None,
                rumps.MenuItem("Quit ClauLi", callback=self._on_quit),
            ]
            self.menu["Status: …"].set_callback(None)
            self._refresh_menu_state()
            # Live status poll — every 3 s.
            self._timer = rumps.Timer(self._tick, 3)
            self._timer.start()

        def _tick(self, _sender):
            state.refresh_from_daemon()
            self._refresh_menu_state()

        def _refresh_menu_state(self):
            # Status line
            label = "Status: "
            if state.last_seen is None:
                label += "starting…"
            elif state.auth_error:
                label += "auth expired"
            elif state.connected:
                label += "● connected"
            else:
                label += "○ disconnected"
            self.menu["Status: …"].title = label
            # Language radio
            lang_menu = self.menu["Language"]
            lang_menu["English"].state  = 1 if state.lang == "en" else 0
            lang_menu["Русский"].state  = 1 if state.lang == "ru" else 0
            # Autostart checkbox
            try:
                self.menu["Start at login"].state = 1 if autostart.is_enabled() else 0
            except Exception:
                self.menu["Start at login"].state = 0

        # ── handlers ──
        def _on_dashboard(self, _sender):
            # Open the dashboard as a native app window (WKWebView). Falls
            # back to the default browser if the WebKit bindings are missing
            # or anything in the Cocoa path raises.
            try:
                import AppKit
                if self._win is None:
                    self._win = _make_native_macos_window(state.dashboard_url, "ClauLi")
                else:
                    # Re-show the existing window and reload so it reflects the
                    # latest config (orientation/colours may have changed).
                    self._win.contentView().reload_(None)
                self._win.makeKeyAndOrderFront_(None)
                AppKit.NSApp.activateIgnoringOtherApps_(True)
            except Exception as e:  # noqa: BLE001 — any Cocoa failure → browser
                print(f"native window unavailable ({e}); opening browser",
                      file=sys.stderr)
                webbrowser.open(state.dashboard_url)

        def _on_dashboard_browser(self, _sender):
            webbrowser.open(state.dashboard_url)

        def _on_refresh(self, _sender):
            state.push_refresh()

        def _set_lang(self, lang: str):
            state.push_lang(lang)
            self._refresh_menu_state()

        def _on_autostart(self, sender):
            new_state = not bool(sender.state)
            try:
                autostart.set_enabled(new_state)
            except Exception as e:
                rumps.alert("ClauLi", f"Failed to update Start at login: {e}")
                return
            sender.state = 1 if autostart.is_enabled() else 0

        def _on_quit(self, _sender):
            try:
                on_quit()
            finally:
                rumps.quit_application()

    ClauliTray().run()


# ---------------------------------------------------------------------------
# Linux / Windows backend — pystray
# ---------------------------------------------------------------------------

def _run_tray_pystray(port: int, on_quit: Callable[[], None]) -> None:
    import pystray  # type: ignore
    from pystray import Menu, MenuItem  # type: ignore

    state = TrayState(port=port)
    image = _build_icon_image()

    def build_menu() -> Menu:
        # State for menu checkmarks is read by lambdas via state instance,
        # so we can rebuild the menu cheaply on every change.
        def lang_is(lang: str):
            return lambda _item: state.lang == lang

        def status_text(_item):
            if state.last_seen is None:
                return "Status: starting…"
            if state.auth_error:
                return "Status: auth expired"
            if state.connected:
                return "Status: ● connected"
            return "Status: ○ disconnected"

        return Menu(
            MenuItem("Open dashboard", lambda _i: webbrowser.open(state.dashboard_url)),
            MenuItem("Refresh", lambda _i: state.push_refresh()),
            Menu.SEPARATOR,
            MenuItem("Language", Menu(
                MenuItem("English",  lambda _i: _set_lang("en"), checked=lang_is("en")),
                MenuItem("Русский",  lambda _i: _set_lang("ru"), checked=lang_is("ru")),
            )),
            MenuItem("Start at login",
                     lambda _i: _toggle_autostart(),
                     checked=lambda _i: autostart.is_enabled()),
            Menu.SEPARATOR,
            MenuItem(status_text, None, enabled=False),
            Menu.SEPARATOR,
            MenuItem("Quit ClauLi", lambda icon, _item: _on_quit_clicked(icon)),
        )

    def _set_lang(lang: str):
        state.push_lang(lang)
        icon.update_menu()

    def _toggle_autostart():
        try:
            autostart.set_enabled(not autostart.is_enabled())
        finally:
            icon.update_menu()

    def _on_quit_clicked(icon):
        try:
            on_quit()
        finally:
            icon.stop()

    icon = pystray.Icon("ClauLi", icon=image, title="ClauLi", menu=build_menu())

    # Background poller — refresh state every 3 s and ask the icon to redraw
    # its menu so the status line / autostart toggle are accurate.
    _stop_poll = threading.Event()

    def _poll():
        while not _stop_poll.is_set():
            if state.refresh_from_daemon():
                try:
                    icon.update_menu()
                except Exception:
                    pass
            _stop_poll.wait(3.0)

    threading.Thread(target=_poll, name="ClauliTrayPoll", daemon=True).start()
    try:
        icon.run()
    finally:
        _stop_poll.set()


# ---------------------------------------------------------------------------
# Public entry
# ---------------------------------------------------------------------------

def run_tray(port: int, on_quit: Callable[[], None]) -> None:
    """Block on the platform-appropriate tray UI.

    When the user picks Quit, `on_quit` is called and this function returns.
    """
    if sys.platform == "darwin":
        try:
            _run_tray_macos(port=port, on_quit=on_quit)
            return
        except ImportError as exc:
            print(
                f"rumps unavailable on macOS ({exc}); falling back to pystray",
                file=sys.stderr,
            )
    # Linux, Windows, or macOS-without-rumps fallback
    _run_tray_pystray(port=port, on_quit=on_quit)
