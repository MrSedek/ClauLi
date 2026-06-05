# Flash ClauLi firmware (first time, over USB)

This bundle flashes the **complete** firmware image to the board's ESP32-C6 over
a USB cable. You only need this once — afterwards you can update wirelessly
(see *OTA* below).

## Steps

1. Connect the board to your computer with a USB-C cable.
2. Run the flasher for your operating system:

   | OS | Command |
   |---|---|
   | macOS / Linux | `./flash.sh`  (optionally pass the port: `./flash.sh /dev/ttyACM0`) |
   | Windows | double-click **`flash.bat`** |

   The script needs **Python 3** — it uses it only to run `esptool`, which it
   installs automatically.
3. When it finishes, the eyes appear on the display within a few seconds. If the
   screen stays blank, unplug and replug the board.

## Files in this bundle

- **`firmware.factory.bin`** — the complete image (bootloader + partitions + app),
  flashed at offset `0x0`. This is what `flash.sh` / `flash.bat` write.

## Updating later — OTA (no cable)

The release also ships **`firmware.bin`** separately. That's the OTA image: open
the device's web configurator → **Firmware OTA** and upload `firmware.bin`. No
cable, no scripts.

> Note: `firmware.bin` (OTA) and `firmware.factory.bin` (USB first-flash) are not
> interchangeable — use the factory image for the initial USB flash.
