# NanoESP32-C6 v1.0 Pinout

Board by MuseLab (V1711). Two USB-C ports: CH343 (serial/flash) and ESP32-C6 native USB.

```
__________
GND |    |    CPU   |    | GND
3V3 |    | ESP32-C6 |    | 2
RST |    |__________|    | 3
  4 |                    | TX  (GPIO 16)
  5 |                    | RX  (GPIO 17)
  6 |                    | 15
  7 |                    | 23
  0 |                    | 22
  1 |                    | 21
  8 |                    | 20
 10 |                    | 19
 11 |                    | 18
 12 |                    | 9
 13 |                    | GND
 5V |                    | 5V
    |  _____    _______  |
    | |USB-C|  | USB-C | |
    | |CH343|  |ESP32C6| | 
```

## GPIO availability

| GPIO | Pin header | Notes |
|------|-----------|-------|
| 0    | Left      | ⚠️ Strapping (VDD_SPI voltage) |
| 1    | Left      | ⚠️ Strapping (SPI boot mode) |
| 2    | Right     | ⚠️ Strapping (VDD_SPI voltage) |
| 3    | Right     | ⚠️ Strapping |
| 4    | Left      | LP GPIO |
| 5    | Left      | LP GPIO |
| 6    | Left      | LP GPIO |
| 7    | Left      | LP GPIO |
| 8    | Left      | ⚠️ RGB LED, strapping (USB download) |
| 9    | Right     | BOOT button |
| 10   | Left      | ✅ Free HP GPIO |
| 11   | Left      | ⚠️ SPI flash WP (may hang in DIO mode) |
| 12   | Left      | ❌ SPI flash CS0 (LOCKED) |
| 13   | Left      | ❌ SPI flash Q (LOCKED) |
| 14   | —         | ❌ SPI flash D (not on header!) |
| 15   | Right     | ❌ SPI flash CLK (on header but LOCKED) |
| 16   | Right (TX)| UART0 TX |
| 17   | Right (RX)| UART0 RX |
| 18   | Right     | ⚠️ SPI flash HD (may hang in DIO mode) |
| 19   | Right     | ❌ SPI flash (not usable) |
| 20   | Right     | ❌ SPI flash (not usable) |
| 21   | Right     | ❌ SPI flash (not usable) |
| 22   | Right     | ❌ SPI flash (not usable) |
| 23   | Right     | ✅ Free HP GPIO |

## Safe GPIOs for SPI LCD

Only **GPIO 10 and 23** are truly free HP GPIOs. All others are either:
- SPI flash (12, 13, 14, 15, 19-22) — using them hangs/crashes the board
- Strapping (0, 1, 2, 3, 8) — affect boot behavior
- LP domain (4, 5, 6, 7) — work as slow GPIO but not for fast SPI
- UART (16, 17) — serial debug
- Possibly flash (11, 18) — may hang in some flash modes

### Working SPI LCD pin assignment

Signal routing uses LP GPIO for slow control signals, HP GPIO for data/clock:

```
LCD DIN (MOSI) → GPIO 10  (HP, fast data)
LCD CLK (SCLK) → GPIO 23  (HP, fast clock)
LCD CS         → GPIO 5   (LP, slow control)
LCD DC         → GPIO 4   (LP, slow control)  
LCD RST        → GPIO 6   (LP, one-time toggle)
LCD BL         → GPIO 7   (LP, simple on/off)
```
