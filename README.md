# Seeed Studio Round Display Clock and Stopwatch

A touch watch face for the [Seeed Studio Round Display for XIAO](https://www.seeedstudio.com/1-28-Round-Touch-Display-for-Seeed-Studio-XIAO-ESP32.html)
(SKU 104030087) stacked on a **XIAO ESP32-C3**.

## Features

- Flicker-free analog watch face (240×240 GC9A01, rendered via a full-frame sprite)
- Real time from the internet: NTP sync over WiFi with automatic hourly re-sync
  (Arizona MST, no DST; falls back to the firmware build time if WiFi is down)
- Day of week and date display
- **Weather face** for Sierra Vista, AZ via [Open-Meteo](https://open-meteo.com/)
  (free API, no key): current temp, conditions, today's high/low, humidity, and
  wind — refreshes every 10 minutes. Location is fixed by latitude/longitude in
  `src/main.cpp` (not auto-detected)
- **Tap to interact** (CHSC6X capacitive touch): each tap cycles through four
  color themes, then **weather**, then a **stopwatch** — tap to start, tap to
  stop, tap to reset and return to the clock

## Setup

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
2. Copy `include/secrets.h.example` to `include/secrets.h` and fill in your
   WiFi name and password (the file is gitignored)
3. Stack the XIAO ESP32-C3 onto the display board and plug in USB
4. Build and flash:

```bash
pio run -t upload
pio device monitor   # optional: watch boot/WiFi/NTP logs at 115200 baud
```

## Hardware pin map

All connections are fixed by the display board — no wiring needed.

| Function | XIAO pin | GPIO |
|---|---|---|
| LCD CS | D1 | 3 |
| LCD DC | D3 | 5 |
| SPI SCK | D8 | 8 |
| SPI MOSI | D10 | 10 |
| Backlight | D6 | 21 |
| I2C SDA (touch/RTC) | D4 | 6 |
| I2C SCL (touch/RTC) | D5 | 7 |
| Touch interrupt | D7 | 20 |
| TF card CS | D2 | 4 |

Only **D0** remains free for your own peripherals (plus the shared I2C bus).

## Note: TFT_eSPI crash fix for the ESP32-C3

Stock TFT_eSPI (2.5.43) crashes on the ESP32-C3 with a `Store access fault`
during `tft.init()`: the IDF defines `REG_SPI_BASE(i)` as `0` for any `i != 2`,
while the library uses `SPI_PORT = SPI2_HOST = 1`, so every SPI register
pointer lands near NULL.

This project fixes it automatically: `tools/patch_tft_espi_c3.py` runs before
every build (see `extra_scripts` in `platformio.ini`) and patches the library's
C3 header to hardwire the base address of GPSPI2 — the only SPI peripheral the
C3 has. The patch is idempotent and re-applies if PlatformIO reinstalls the
library.

## Project layout

| Path | Purpose |
|---|---|
| `src/main.cpp` | All application code, heavily commented |
| `platformio.ini` | Board config + TFT_eSPI display settings (build flags) |
| `include/secrets.h.example` | Template for WiFi credentials |
| `tools/patch_tft_espi_c3.py` | TFT_eSPI ESP32-C3 crash fix (pre-build) |
| `tools/serial_capture.py` | Helper: dump serial output for N seconds |

## Changing the weather location

Edit these three constants near the top of `src/main.cpp`:

```cpp
static const float WEATHER_LAT = 31.55f;     // your latitude
static const float WEATHER_LON = -110.28f;   // your longitude
static const char *WEATHER_PLACE = "SIERRA VISTA";  // label on screen
```

Right-click your town on Google Maps to copy coordinates.

## Ideas for next steps

- Persist time to the on-board PCF8563 RTC (I2C `0x51`) so it survives
  power-off with the backup battery
- Battery level display (LiPo connects via the JST 1.25 connector)
- Log or load data from the TF card slot (shares SPI, CS on D2)
