# Context — ESP32-C6 + ST7789V2 Display

## Current session
Full data pipeline working: daemon → BLE → ESP32 → JSON parse → LVGL UI render.
Colors confirmed correct with `ips=true`.

## Critical GPIO constraints on NanoESP32-C6 v1.0 (MuseLab)
- **GPIO 12, 13, 14, 15** = SPI flash — LOCKED, touching them hangs/crashes the board!
- **GPIO 19, 20, 21, 22** = also SPI flash — not usable
- **GPIO 11** = SPI flash WP — hangs board in DIO flash mode
- **GPIO 18** = SPI flash HD — hangs board in DIO flash mode
- **GPIO 16, 17** = UART0 TX/RX (serial debug via CH343)
- **GPIO 0, 1, 2, 3** = strapping pins
- **GPIO 8** = RGB LED + strapping
- **GPIO 9** = BOOT button
- **Only free HP GPIOs: 10, 23** — that's it!
- **LP GPIOs 4, 5, 6, 7** — work as slow digital I/O, NOT for fast SPI

## Working display wiring
```
LCD DIN (MOSI) → GPIO 10  (HP GPIO — fast data)
LCD CLK (SCLK) → GPIO 23  (HP GPIO — fast clock)
LCD CS         → GPIO 5   (LP GPIO — slow control)
LCD DC         → GPIO 4   (LP GPIO — slow control)
LCD RST        → GPIO 6   (LP GPIO — one-time toggle)
LCD BL         → GPIO 7   (LP GPIO — simple on/off)
```

## Display init
- ST7789V2, 240×280 in 240×320 controller
- **`ips=true`** — ST7789V2 1.69" panel needs color inversion (INVON 0x21). Without it, colors are inverted (red↔cyan, green↔magenta, blue↔yellow).
- `col_offset=0`, `row_offset=20`
- Arduino_HWSPI (SPIClass) — NOT Arduino_ESP32SPI (broken on C6)

## BLE — fully working
- Daemon connects, sends JSON, ESP32 receives and parses correctly
- Data confirmed: `{"s":7,"sr":279,"w":5,"wr":2529,"st":"allowed","ok":true}`
- Parsed on ESP32: `s=7.0% sr=279 w=5.0% wr=2529 st=allowed`
- BLE state transitions: INIT → ADVERTISING → CONNECTED (ble=2)
- ESP32 fires refresh request on subscribe if no data yet
- Python daemon uses anthropic SDK for auto token refresh

## Serial debug logging (still active)
- `BLE: RX N bytes: <json>` — when data arrives
- `DATA: processing: <json>` — in main loop
- `DATA: parsed OK s=X% sr=N w=Y% wr=M st=Z` — parse success
- `STATUS: ble=N valid=N s=X w=Y` — every 15s
- `BLE state: N` — on state change

## Open issues
- Rotation via ST7789 MADCTL (no IMU on C6 — double-press BOOT to cycle)
- LVGL render buffers — internal SRAM only (no PSRAM on C6)
- Flash at 95.4% — tight, may need optimization later
- Debug Serial logging still active — can be removed to save flash
