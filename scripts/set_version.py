"""Patch VERSION = "..." in a Python source file.
Usage: python3 scripts/set_version.py <file> <version>
"""
import re
import sys
from pathlib import Path

if len(sys.argv) != 3:
    sys.exit("Usage: set_version.py <file> <version>")

target = Path(sys.argv[1])
version = sys.argv[2]

original = target.read_text()
patched = re.sub(r'^VERSION\s*=\s*"[^"]*"', f'VERSION = "{version}"', original, flags=re.M)

if patched == original:
    print(f"No VERSION line found in {target} — skipping", file=sys.stderr)
else:
    target.write_text(patched)
    print(f"Set VERSION = \"{version}\" in {target}")
