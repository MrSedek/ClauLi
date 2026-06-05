@echo off
REM ── ClauLi USB flasher (Windows) ────────────────────────────────────────
REM Flashes the complete firmware image to an ESP32-C6 over USB.
REM Requires Python 3 in PATH (used only to run esptool, installed below).
REM
REM   flash.bat            (double-click, or run from a terminal)
REM
setlocal
where python >nul 2>nul
if errorlevel 1 (
  echo error: Python 3 not found in PATH. Install it from https://python.org and re-run.
  pause
  exit /b 1
)

echo Installing esptool (one-time)...
python -m pip install --quiet --upgrade esptool

echo Flashing firmware.factory.bin at offset 0x0...
python -m esptool --chip esp32c6 write_flash 0x0 "%~dp0firmware.factory.bin"
if errorlevel 1 (
  echo Flashing failed. Check the cable / port and try again.
  pause
  exit /b 1
)

echo Done. If the screen stays blank, unplug and replug the board.
pause
