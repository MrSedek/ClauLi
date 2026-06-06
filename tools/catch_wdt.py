#!/usr/bin/env python3
"""Catch & decode ESP32-C6 panic / Task-Watchdog backtraces over USB serial.

The reconnect churn is dominated by the device rebooting: `[BOOT]
reason=TASK_WDT(6)` — the main loop hangs >15 s and the Task WDT panic-reboots
it. The hang is intermittent (triggered during reconnect churn, not steady
state), so a fixed-length capture usually misses it. Leave this running; when
the next watchdog fires it logs the panic AND resolves every code address to
`function  file:line` via riscv32-esp-elf-addr2line against the built ELF — so
we can pin exactly where the loop is stuck and fix it.

Usage:
    python3 tools/catch_wdt.py [port] [seconds]
        port     default: first /dev/cu.usbmodem*
        seconds  default: 0 = run until Ctrl-C

Needs pyserial (any Python with bleak/platformio has it):
    ~/.platformio/penv/bin/python3 tools/catch_wdt.py
"""
import glob
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "firmware/.pio/build/esp32c6/firmware.elf")
A2L = os.path.expanduser(
    "~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-addr2line"
)
LOG = "/tmp/clauli_wdt_catch.log"

# C6 code lives in 0x40xxxxxx (IRAM) / 0x42xxxxxx (flash XIP).
ADDR = re.compile(r"0x4[0-2][0-9a-fA-F]{6}")
# Markers that open a panic/WDT region worth decoding.
PANIC = re.compile(
    r"task_wdt|watchdog|panic|Guru Meditation|register dump|MEPC|abort\(\)|"
    r"Backtrace|StackCanary|Stack smashing",
    re.I,
)
# Markers that mean the chip rebooted → close + decode the region we collected.
RESET = re.compile(r"rst:0x|ESP-ROM|\[BOOT\] reason=|Rebooting", re.I)


def decode(addrs):
    addrs = list(dict.fromkeys(addrs))  # de-dup, keep order
    if not addrs:
        return
    if not os.path.exists(ELF):
        print(f"\n[catch_wdt] ELF missing ({ELF}); raw addrs: {' '.join(addrs)}")
        return
    if not os.path.exists(A2L):
        print(f"\n[catch_wdt] addr2line missing; raw addrs: {' '.join(addrs)}")
        return
    out = subprocess.run(
        [A2L, "-pfiaC", "-e", ELF, *addrs], capture_output=True, text=True
    ).stdout
    print("\n===================== DECODED WDT/PANIC BACKTRACE =====================")
    print(out.rstrip())
    print("======================================================================\n")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else None
    if not port:
        cands = sorted(glob.glob("/dev/cu.usbmodem*")) or sorted(
            glob.glob("/dev/ttyACM*")
        )
        port = cands[0] if cands else None
    if not port:
        sys.exit("no serial port found; pass one explicitly")
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0

    try:
        import serial
    except Exception as e:  # noqa: BLE001
        sys.exit(f"pyserial required: {e}")

    s = serial.Serial(port, 115200, timeout=0.4)
    print(
        f"[catch_wdt] listening {port} @115200 → {LOG}  "
        f"(elf {'ok' if os.path.exists(ELF) else 'MISSING'}, "
        f"{'until Ctrl-C' if dur == 0 else f'{dur:.0f}s'})"
    )
    in_panic = False
    addrs = []
    t0 = time.time()
    with open(LOG, "a", buffering=1) as f:
        try:
            while dur == 0 or time.time() - t0 < dur:
                d = s.read(4096)
                if not d:
                    continue
                txt = d.decode("utf-8", "replace")
                sys.stdout.write(txt)
                sys.stdout.flush()
                f.write(txt)
                for line in txt.splitlines():
                    if PANIC.search(line):
                        in_panic = True
                    if in_panic:
                        addrs += ADDR.findall(line)
                    # A reset marker ends the panic burst → decode what we have.
                    if RESET.search(line) and addrs:
                        decode(addrs)
                        in_panic, addrs = False, []
        except KeyboardInterrupt:
            pass
    if addrs:
        decode(addrs)
    s.close()


if __name__ == "__main__":
    main()
