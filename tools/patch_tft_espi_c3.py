"""PlatformIO pre-build script: fix TFT_eSPI on ESP32-C3.

IDF defines REG_SPI_BASE(i) as 0 for any i != 2, but TFT_eSPI uses
SPI_PORT = SPI2_HOST = 1, so every SPI register pointer in the library
ends up near NULL and the firmware crashes in tft.init() with a
"Store access fault" (MTVAL 0x10). The C3 has only one usable SPI
peripheral (GPSPI2), so the base address can simply be hardwired.
"""

from pathlib import Path

Import("env")  # noqa: F821

MARKER = "// PATCHED_REG_SPI_BASE"
ANCHOR = "#define SPI_PORT SPI2_HOST"
PATCH = (
    ANCHOR
    + "\n"
    + MARKER
    + ": IDF's REG_SPI_BASE(i) is 0 unless i==2; C3 only has GPSPI2\n"
    + "#undef REG_SPI_BASE\n"
    + "#define REG_SPI_BASE(i) DR_REG_SPI2_BASE\n"
)

header = Path(env.subst(
    "$PROJECT_LIBDEPS_DIR/$PIOENV/TFT_eSPI/Processors/TFT_eSPI_ESP32_C3.h"
))

if header.exists():
    src = header.read_text()
    if MARKER not in src:
        header.write_text(src.replace(ANCHOR, PATCH, 1))
        print("Patched TFT_eSPI_ESP32_C3.h: REG_SPI_BASE fixed for ESP32-C3")
