"""Cross-platform autostart-at-login management for ClauLi.

Each backend exposes the same two functions:
    is_enabled() -> bool
    set_enabled(enable: bool, exec_path: Optional[str] = None) -> None

The right backend is selected from `sys.platform`; an unsupported platform
returns a no-op stub whose `is_enabled()` is always False so the tray menu
hides / disables the toggle gracefully.

`exec_path` is the path to the launcher (the app bundle on macOS, the .exe
on Windows, the binary or `python script.py` on Linux). When None, we
auto-derive it from `sys.argv` and `sys.executable` — works for both PyInstaller
frozen bundles (sys.frozen is True, sys.executable points at the bundled
binary) and plain `python claude_usage_daemon.py --tray` development runs.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

APP_ID = "com.sedek.clauli"
APP_NAME = "ClauLi"


# ---------------------------------------------------------------------------
# Default exec path resolution
# ---------------------------------------------------------------------------

def _default_exec_command() -> list[str]:
    """Return the argv we'd want the OS to launch on login.

    - PyInstaller / py2app frozen: a single binary path. `sys.frozen` is
      True; `sys.executable` is the wrapped binary (NOT the system python).
    - Development run (`python claude_usage_daemon.py --tray`):
      [<python>, <abs script>, "--tray"]. We force `--tray` because that's
      the only mode anyone would want auto-started on login — running the
      foreground daemon at boot would dump logs to a non-existent terminal.
    """
    if getattr(sys, "frozen", False):
        return [sys.executable]
    script = Path(sys.argv[0]).resolve() if sys.argv and sys.argv[0] else None
    if script is None or not script.exists():
        script = Path(__file__).resolve().parent / "claude_usage_daemon.py"
    return [sys.executable, str(script), "--tray"]


def _shell_quote(parts: list[str]) -> str:
    # Quote each element with shlex so spaces / unicode in paths survive the
    # round-trip through a launchd plist / .desktop file / registry value.
    import shlex
    return " ".join(shlex.quote(p) for p in parts)


# ---------------------------------------------------------------------------
# macOS — LaunchAgent (~/Library/LaunchAgents/com.sedek.clauli.plist)
# ---------------------------------------------------------------------------

def _mac_plist_path() -> Path:
    return Path.home() / "Library" / "LaunchAgents" / f"{APP_ID}.plist"


def _mac_build_plist(argv: list[str]) -> str:
    # Plist needs XML-escaping; argv elements are user-controlled paths so
    # we run them through xml.sax.saxutils.escape to be safe.
    from xml.sax.saxutils import escape
    argv_xml = "\n".join(f"\t\t<string>{escape(a)}</string>" for a in argv)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTD/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
\t<key>Label</key>
\t<string>{APP_ID}</string>
\t<key>ProgramArguments</key>
\t<array>
{argv_xml}
\t</array>
\t<key>RunAtLoad</key>
\t<true/>
\t<key>KeepAlive</key>
\t<false/>
\t<key>ProcessType</key>
\t<string>Interactive</string>
</dict>
</plist>
"""


def _mac_is_enabled() -> bool:
    return _mac_plist_path().exists()


def _mac_set_enabled(enable: bool, argv: list[str]) -> None:
    path = _mac_plist_path()
    if enable:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(_mac_build_plist(argv))
        # `launchctl load` is the official way but it's deprecated in favour
        # of `bootstrap` on newer macOS. Try modern first, fall back to load.
        uid = os.getuid()
        for cmd in (
            ["launchctl", "bootstrap", f"gui/{uid}", str(path)],
            ["launchctl", "load", str(path)],
        ):
            try:
                r = subprocess.run(cmd, capture_output=True, timeout=5)
                if r.returncode == 0:
                    break
            except (subprocess.SubprocessError, FileNotFoundError):
                continue
    else:
        if path.exists():
            uid = os.getuid()
            for cmd in (
                ["launchctl", "bootout", f"gui/{uid}/{APP_ID}"],
                ["launchctl", "unload", str(path)],
            ):
                try:
                    subprocess.run(cmd, capture_output=True, timeout=5)
                except (subprocess.SubprocessError, FileNotFoundError):
                    pass
            try:
                path.unlink()
            except OSError:
                pass


# ---------------------------------------------------------------------------
# Linux — XDG Autostart (~/.config/autostart/clauli.desktop)
# ---------------------------------------------------------------------------

def _linux_desktop_path() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "autostart" / "clauli.desktop"


def _linux_build_desktop(argv: list[str]) -> str:
    exec_line = _shell_quote(argv)
    return (
        "[Desktop Entry]\n"
        "Type=Application\n"
        f"Name={APP_NAME}\n"
        f"Exec={exec_line}\n"
        "Terminal=false\n"
        "X-GNOME-Autostart-enabled=true\n"
        "Categories=Utility;\n"
        "Comment=Claude Code usage monitor — ESP32 BLE companion\n"
    )


def _linux_is_enabled() -> bool:
    return _linux_desktop_path().exists()


def _linux_set_enabled(enable: bool, argv: list[str]) -> None:
    path = _linux_desktop_path()
    if enable:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(_linux_build_desktop(argv))
        try:
            path.chmod(0o755)  # Some DEs only honour the entry if it's +x
        except OSError:
            pass
    else:
        if path.exists():
            try:
                path.unlink()
            except OSError:
                pass


# ---------------------------------------------------------------------------
# Windows — HKCU\Software\Microsoft\Windows\CurrentVersion\Run
# ---------------------------------------------------------------------------

_WIN_REG_PATH = r"Software\Microsoft\Windows\CurrentVersion\Run"
_WIN_VALUE_NAME = APP_NAME  # "ClauLi"


def _win_quote_command(argv: list[str]) -> str:
    # Windows Run-key entries are a single string parsed by CommandLineToArgvW.
    # Wrap each path-like arg in double quotes; escape internal quotes by
    # doubling. This matches the cmd.exe convention.
    def q(s: str) -> str:
        if " " in s or "\t" in s:
            return '"' + s.replace('"', '""') + '"'
        return s
    return " ".join(q(p) for p in argv)


def _win_is_enabled() -> bool:
    try:
        import winreg  # type: ignore
    except ImportError:
        return False
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _WIN_REG_PATH) as key:
            winreg.QueryValueEx(key, _WIN_VALUE_NAME)
            return True
    except FileNotFoundError:
        return False
    except OSError:
        return False


def _win_set_enabled(enable: bool, argv: list[str]) -> None:
    try:
        import winreg  # type: ignore
    except ImportError:
        return
    if enable:
        cmd = _win_quote_command(argv)
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, _WIN_REG_PATH, 0,
                                 winreg.KEY_SET_VALUE) as key:
            winreg.SetValueEx(key, _WIN_VALUE_NAME, 0, winreg.REG_SZ, cmd)
    else:
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _WIN_REG_PATH, 0,
                                 winreg.KEY_SET_VALUE) as key:
                winreg.DeleteValue(key, _WIN_VALUE_NAME)
        except FileNotFoundError:
            pass
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def is_supported() -> bool:
    return sys.platform in ("darwin", "linux", "win32")


def is_enabled() -> bool:
    if sys.platform == "darwin":
        return _mac_is_enabled()
    if sys.platform == "linux":
        return _linux_is_enabled()
    if sys.platform == "win32":
        return _win_is_enabled()
    return False


def set_enabled(enable: bool, exec_command: Optional[list[str]] = None) -> None:
    argv = exec_command if exec_command is not None else _default_exec_command()
    if sys.platform == "darwin":
        _mac_set_enabled(enable, argv)
    elif sys.platform == "linux":
        _linux_set_enabled(enable, argv)
    elif sys.platform == "win32":
        _win_set_enabled(enable, argv)
    # else: silent no-op on unsupported platforms


if __name__ == "__main__":
    # Quick CLI for manual testing:
    #   python autostart.py status
    #   python autostart.py on
    #   python autostart.py off
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    if cmd == "status":
        print(f"supported = {is_supported()}")
        print(f"enabled   = {is_enabled()}")
        print(f"argv      = {_default_exec_command()}")
    elif cmd == "on":
        set_enabled(True)
        print(f"enabled — argv = {_default_exec_command()}")
    elif cmd == "off":
        set_enabled(False)
        print("disabled")
    else:
        print("usage: python autostart.py [status|on|off]", file=sys.stderr)
        sys.exit(2)
