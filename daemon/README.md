# ClauLi daemon

Three ways to run, plus an automated cross-platform release pipeline.

## Modes

| Mode | Command | What runs |
|---|---|---|
| **Foreground daemon** (existing, used by `daemon.sh`) | `python claude_usage_daemon.py` | aiohttp + BLE poll loop, logs to stdout |
| **Tray app** (new) | `python claude_usage_daemon.py --tray` | Same daemon on a background thread + menu-bar / system-tray UI |
| **CLI command** (existing) | `python claude_usage_daemon.py --status` (and `--refresh`, `--screen`, `--action`, `--ui`) | One-shot HTTP call to a running daemon |

Native bundles (see below) always launch in tray mode.

## Tray menu

```
ClauLi
├─ Open dashboard          → native app window (WKWebView) on http://localhost:8765/
├─ Open in browser         → same URL in the default browser
├─ Refresh                  → POST /api/refresh (force a poll right now)
├──────────────
├─ Language ▸               (radio)
│   ✓ English
│     Русский
├─ Start at login           (checkbox — toggles autostart, see below)
├──────────────
├─ Status: ● connected      (live, polled every 3 s)
├──────────────
└─ Quit ClauLi
```

State (BLE connection, language, autostart flag) lives in the running daemon
and the host OS. The tray polls `/api/status` every 3 s so the web UI and
the tray stay in sync — change language in either surface, both update.

## Autostart at login (per-OS)

| OS | Mechanism | File |
|---|---|---|
| macOS | LaunchAgent | `~/Library/LaunchAgents/com.sedek.clauli.plist` |
| Linux | XDG Autostart | `~/.config/autostart/clauli.desktop` |
| Windows | HKCU Run key | `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run\ClauLi` |

Managed by `autostart.py` — toggled via the **Start at login** tray item.
Each backend is independent; the file/key is removed cleanly when you
toggle it off. You can also drive it manually:

```bash
python autostart.py status     # show current state + argv that would be run
python autostart.py on
python autostart.py off
```

## Building native bundles locally

### macOS — `.app` bundle (py2app)

Use a **framework** Python (the python.org installer or Homebrew's
`python-tk`-free build) — py2app needs `Python.framework`, not the
PlatformIO/conda interpreters that happen to be first on `$PATH`.

```bash
cd daemon
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt -r requirements-build.txt   # setuptools + py2app
python setup_macos.py py2app -A      # alias mode (fast, for iteration)
open dist/ClauLi.app                  # launches; check menu bar for the icon

# Full bundle for distribution:
python setup_macos.py py2app
ditto -c -k --sequesterRsrc --keepParent dist/ClauLi.app dist/ClauLi-macOS.zip
```

`LSUIElement = true` in the Info.plist means **no Dock icon, no app
window** — only the menu-bar icon. Quitting from the menu fully exits
the process.

### Linux / Windows — PyInstaller

```bash
cd daemon
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt -r requirements-build.txt   # setuptools + pyinstaller
pyinstaller --clean --noconfirm clauli.spec
./dist/clauli/clauli --tray              # Linux
dist\clauli\clauli.exe --tray            # Windows
```

`clauli.spec` is shared between Linux and Windows; PyInstaller picks the
right console/icon defaults per-platform.

## Release pipeline (`.github/workflows/release.yml`)

Triggered by pushing a `v*` tag (or manually from the Actions tab):

```bash
git tag v0.1.0
git push --tags
```

Five jobs run in parallel:

| Job | Runner | Output |
|---|---|---|
| `firmware` | ubuntu-22.04 | `firmware.bin`, `partitions.bin`, `bootloader.bin` |
| `macos` | macos-14 (arm64) | `ClauLi-macOS.zip` |
| `linux` | ubuntu-22.04 | `clauli-linux-x86_64.tar.gz` |
| `windows` | windows-latest | `ClauLi-windows-x86_64.zip` |
| `release` | ubuntu-22.04 (needs all four) | GitHub Release with all artefacts attached |

The release body includes ready-to-paste install instructions for each OS.

### Caveats

- **No code-signing yet.** macOS users see "unidentified developer" on
  first launch → Right-click → Open once. Windows users see SmartScreen
  → *More info → Run anyway*. Once we have an Apple Developer account
  ($99/year), the macOS job gains a `codesign` + `notarytool` step.
- **BLE permission on macOS** is granted on first run via the standard
  prompt (we include `NSBluetoothAlwaysUsageDescription` in the plist).
- **Bundle sizes** are ~50 MB each (Python interpreter + libs) — normal
  for py2app / PyInstaller.

## Module layout

```
daemon/
├── claude_usage_daemon.py    main daemon (HTTP + BLE + Anthropic poll)
├── tray.py                   menu-bar UI (rumps on macOS, pystray elsewhere)
├── autostart.py              cross-platform login-item / Run-key / .desktop
├── macos_entry.py            py2app entry — forces --tray
├── setup_macos.py            py2app config
├── clauli.spec               PyInstaller spec (Linux / Windows)
├── daemon.sh                 lifecycle wrapper for foreground mode
├── requirements.txt          runtime deps
├── requirements-build.txt    build-only deps (setuptools, py2app/pyinstaller)
└── web/                      static UI served at http://localhost:8765/
    └── index.html
```
