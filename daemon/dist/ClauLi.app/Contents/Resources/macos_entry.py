"""macOS .app bundle entry point — forces tray mode regardless of argv.

When py2app launches the bundle there's no terminal to receive flags,
so we monkey-patch sys.argv to include --tray and hand off to the
existing main(). All other CLI behaviour (--status, --refresh, ...) is
still available via `daemon/daemon.sh` or `python claude_usage_daemon.py`.
"""
import sys

if "--tray" not in sys.argv:
    sys.argv.append("--tray")

from claude_usage_daemon import main

if __name__ == "__main__":
    main()
