// ============================================================================
// Analog watch face for the Seeed Round Display for XIAO (1.28" GC9A01)
// running on a XIAO ESP32-C3.
//
// What this program does, in plain words:
//
//   1. At power-on it connects to your WiFi and fetches the real time from
//      the internet (NTP). Until that arrives, it uses the date/time the
//      firmware was compiled, so the face is roughly right immediately.
//   2. Forty times per second it draws a complete watch face (ring, tick
//      marks, hands, digital time, day, date) into an off-screen image
//      buffer called a "sprite", then copies that buffer to the screen in
//      one go. Drawing off-screen first is what makes it flicker-free.
//   3. It watches the touch controller. Each tap anywhere on the glass
//      advances one step through a cycle: the four color themes, then a
//      stopwatch screen (tap to start, tap to stop, tap to reset & return).
//
// Hardware connections (all fixed by the display board, nothing to wire):
//   - Display:  SPI bus  (config lives in platformio.ini as build flags)
//   - Touch:    I2C bus, plus an "interrupt" pin that goes LOW when touched
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>    // ESP32 WiFi (used only to reach the NTP time servers)
#include <Wire.h>    // I2C bus (talks to the touch controller)
#include <time.h>    // standard C time functions (time, localtime, mktime)

#include <TFT_eSPI.h>  // display driver library

#include "esp_sntp.h"  // lets us register a "time was synced" callback
#include "secrets.h"   // your WiFi name + password (include/secrets.h)

// Timezone rule string in POSIX format:
//   PST8PDT  = Pacific Standard Time, 8 hours behind UTC, with DST ("PDT")
//   M3.2.0   = DST starts 2nd Sunday of March
//   M11.1.0  = DST ends 1st Sunday of November
// The C time library uses this to convert UTC into your local wall time.
static const char *TZ_INFO = "PST8PDT,M3.2.0,M11.1.0";

// ================================ TOUCH =====================================
// The display has a CHSC6X capacitive touch chip on the I2C bus.
// It signals "finger detected" by pulling its interrupt pin LOW.
// Only then do we ask it (over I2C) where the finger is.

static const uint8_t TOUCH_I2C_ADDR = 0x2E;  // chip's fixed I2C address
static const uint8_t TOUCH_INT_PIN = D7;     // LOW = a touch is happening

// Reads one touch point. Returns true and fills x/y (screen pixels, 0-239)
// only while a finger is on the glass.
static bool readTouch(int16_t &x, int16_t &y) {
  // Cheap check first: no interrupt = no finger = don't bother the I2C bus.
  if (digitalRead(TOUCH_INT_PIN) != LOW) return false;

  // The chip reports 5 bytes: [number of fingers, ?, x, ?, y]
  uint8_t data[5];
  if (Wire.requestFrom(TOUCH_I2C_ADDR, (uint8_t)5) != 5) return false;
  for (uint8_t i = 0; i < 5; i++) data[i] = Wire.read();

  if (data[0] != 1) return false;  // we only handle exactly one finger
  x = data[2];
  y = data[4];
  return true;
}

// =============================== DISPLAY ====================================

TFT_eSPI tft;               // the real screen (240x240 pixels)
TFT_eSprite frame(&tft);    // off-screen buffer we draw each frame into

static const int16_t CX = 120, CY = 120;  // center of the round screen

// A color theme for the face. Colors are 16-bit "RGB565" values: 5 bits red,
// 6 bits green, 5 bits blue packed into one number (e.g. 0xF800 = pure red).
struct Theme {
  uint16_t bg;      // background fill
  uint16_t ring;    // outer ring
  uint16_t ticks;   // small minute tick marks
  uint16_t hands;   // hour/minute hands + the 12 big tick marks
  uint16_t accent;  // second hand, center dot, day-of-week text
  uint16_t text;    // digital time + date text
};

// Tap the screen to cycle through these.
static const Theme THEMES[] = {
    {TFT_BLACK, 0x2945, 0x7BEF, TFT_WHITE, 0xFD20, 0xBDF7},  // amber on dark
    {TFT_BLACK, 0x0926, 0x34D9, TFT_WHITE, 0x07FF, 0x9FF3},  // cyan
    {0x0841, 0x29A8, 0x8C71, 0xFFDF, 0xF8B2, 0xCE9F},        // pink slate
    {0xFFFF, 0xCE79, 0x6B4D, 0x0000, 0xF800, 0x4208},        // light / red
};
static const uint8_t THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);
static uint8_t themeIdx = 0;  // which theme is currently shown

// ================================ MODES =====================================
// Each tap moves one step through this cycle:
//
//   theme 1 -> theme 2 -> theme 3 -> theme 4 -> STOPWATCH (ready)
//        -> tap: stopwatch RUNNING -> tap: stopwatch STOPPED
//        -> tap: reset and back to theme 1
enum Mode : uint8_t {
  MODE_CLOCK,    // normal watch face (taps cycle the color themes)
  SW_READY,      // stopwatch screen showing 00:00.0, waiting to start
  SW_RUNNING,    // stopwatch counting up
  SW_STOPPED,    // stopwatch frozen at the final time
};
static Mode mode = MODE_CLOCK;

static uint32_t swStartMs = 0;    // millis() value when the stopwatch started
static uint32_t swFrozenMs = 0;   // elapsed time captured when stopped

// How many milliseconds the stopwatch currently shows.
static uint32_t stopwatchMs() {
  if (mode == SW_RUNNING) return millis() - swStartMs;
  if (mode == SW_STOPPED) return swFrozenMs;
  return 0;  // SW_READY
}

// ================================= TIME =====================================
// The ESP32 keeps a "system clock" (seconds since Jan 1 1970, aka an epoch).
// We seed it at boot from the moment this firmware was COMPILED -- the
// compiler bakes that moment into the magic constants __DATE__ and __TIME__.
// That makes the face approximately right even before WiFi works. Once NTP
// answers, the system clock is silently corrected to true internet time.

static time_t parseBuildTime() {
  static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  struct tm t = {};    // a broken-down date/time (year, month, day, ...)
  char mon[4] = {};
  // __DATE__ looks like "Jun 11 2026" and __TIME__ like "12:05:33".
  sscanf(__DATE__, "%3s %d %d", mon, &t.tm_mday, &t.tm_year);
  sscanf(__TIME__, "%d:%d:%d", &t.tm_hour, &t.tm_min, &t.tm_sec);
  // Find the month name's position in the list above -> month number 0-11.
  t.tm_mon = (strstr(MONTHS, mon) - MONTHS) / 3;
  t.tm_year -= 1900;   // struct tm counts years from 1900 (C convention)
  t.tm_isdst = -1;     // "you figure out if DST applies"
  return mktime(&t);   // convert broken-down local time -> epoch seconds
}

// Current time as epoch seconds, straight from the system clock.
static time_t clockSeconds() {
  return time(nullptr);
}

// =============================== DRAWING ====================================

// Draws one watch hand into the sprite.
//   angleDeg   - where the hand points: 0 = twelve o'clock, 90 = three, etc.
//   length     - how far the hand reaches from the center (pixels)
//   backLength - short "tail" sticking out the opposite side (pixels)
//   width      - thickness at the center, tapering to a point
// drawWedgeLine draws a tapered, anti-aliased (smooth-edged) line.
static void drawHand(float angleDeg, float length, float backLength,
                     float width, uint16_t color) {
  // Screen angles: 0 degrees in cos/sin points RIGHT (3 o'clock), but for a
  // clock we want 0 to point UP, so subtract 90. DEG_TO_RAD converts
  // degrees to radians, which is what cosf/sinf expect.
  float a = (angleDeg - 90.0f) * DEG_TO_RAD;
  float c = cosf(a), s = sinf(a);
  frame.drawWedgeLine(CX - backLength * c, CY - backLength * s,  // tail end
                      CX + length * c, CY + length * s,          // tip
                      width, 1.0f, color);
}

// Draws the parts shared by every screen: background, outer ring, and the
// 60 tick marks around the edge (every 5th one is longer and bolder).
static void drawRingAndTicks(const Theme &t) {
  // Start every frame from a clean background.
  frame.fillSprite(t.bg);

  // Outer ring (two circles, one pixel apart, looks slightly thicker)
  frame.drawSmoothCircle(CX, CY, 119, t.ring, t.bg);
  frame.drawSmoothCircle(CX, CY, 118, t.ring, t.bg);

  for (int i = 0; i < 60; i++) {
    float a = i * 6.0f * DEG_TO_RAD;  // 360 degrees / 60 ticks = 6 per tick
    float c = cosf(a), s = sinf(a);
    bool major = (i % 5) == 0;
    float r1 = major ? 102.0f : 110.0f;        // longer ticks start further in
    frame.drawWedgeLine(CX + r1 * s, CY - r1 * c,        // inner end
                        CX + 115.0f * s, CY - 115.0f * c,  // outer end
                        major ? 2.5f : 1.0f, 1.0f,
                        major ? t.hands : t.ticks);
  }
}

// Draws the normal watch face into the sprite, then copies the finished
// sprite to the actual screen. Called ~20 times per second.
static void drawClock() {
  const Theme &t = THEMES[themeIdx];
  drawRingAndTicks(t);

  // --- Get the current local time, split into hours/minutes/seconds ---
  time_t now = clockSeconds();
  struct tm tmNow;
  localtime_r(&now, &tmNow);  // epoch seconds -> local year/month/day/h/m/s
  int s = tmNow.tm_sec;
  int m = tmNow.tm_min;
  int h = tmNow.tm_hour % 12;  // analog face only shows 12 hours

  // --- Text: digital time below center, day + date above center ---
  static const char *DAYS[] = {"SUN", "MON", "TUE", "WED",
                               "THU", "FRI", "SAT"};
  static const char *MONTHS[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                 "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  frame.setTextDatum(MC_DATUM);  // position text by its middle-center point
  frame.setTextColor(t.text, t.bg);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", (h == 0 ? 12 : h), m);
  frame.drawString(buf, CX, CY + 52, 4);   // "4" = built-in font number
  frame.setTextColor(t.accent, t.bg);
  frame.drawString(DAYS[tmNow.tm_wday], CX, CY - 56, 2);  // e.g. "THU"
  snprintf(buf, sizeof(buf), "%s %d", MONTHS[tmNow.tm_mon], tmNow.tm_mday);
  frame.setTextColor(t.text, t.bg);
  frame.drawString(buf, CX, CY - 38, 2);   // e.g. "JUN 11"

  // --- The three hands ---
  // Angles: the second hand moves 6 degrees per second (360/60). The minute
  // hand moves 6 per minute plus a tiny bit each second; the hour hand moves
  // 30 per hour (360/12) plus a bit each minute -- that's what makes them
  // creep smoothly instead of jumping.
  float sa = s * 6.0f;
  float ma = m * 6.0f + s * 0.1f;
  float ha = h * 30.0f + m * 0.5f;
  drawHand(ha, 58, 12, 4.0f, t.hands);    // hour: short and thick
  drawHand(ma, 88, 14, 3.0f, t.hands);    // minute: longer
  drawHand(sa, 100, 22, 1.5f, t.accent);  // second: longest and thinnest

  // --- Center hub: a small accent dot with a tiny hole ---
  frame.fillSmoothCircle(CX, CY, 5, t.accent, t.bg);
  frame.fillSmoothCircle(CX, CY, 2, t.bg, t.accent);

  // Copy the finished frame to the real screen, starting at top-left (0,0).
  frame.pushSprite(0, 0);
}

// Draws the stopwatch screen. Reuses the current theme's colors so the
// stopwatch matches whatever dial color you last picked.
static void drawStopwatch() {
  const Theme &t = THEMES[themeIdx];
  drawRingAndTicks(t);

  uint32_t ms = stopwatchMs();
  uint32_t tenths = (ms / 100) % 10;
  uint32_t secs = (ms / 1000) % 60;
  uint32_t mins = (ms / 60000) % 100;  // wraps after 99 minutes

  // A sweeping accent hand showing the current second within the minute,
  // just like a mechanical stopwatch needle.
  float needleAngle = (ms % 60000) * (360.0f / 60000.0f);
  drawHand(needleAngle, 100, 16, 1.5f, t.accent);

  // Label on top, big elapsed time in the middle, tap hint below.
  frame.setTextDatum(MC_DATUM);
  frame.setTextColor(t.accent, t.bg);
  frame.drawString("STOPWATCH", CX, CY - 56, 2);

  char buf[10];
  snprintf(buf, sizeof(buf), "%02u:%02u.%u", mins, secs, tenths);
  frame.setTextColor(t.text, t.bg);
  frame.drawString(buf, CX, CY, 6);  // font 6 = large 48-pixel digits

  const char *hint = (mode == SW_READY)     ? "TAP TO START"
                     : (mode == SW_RUNNING) ? "TAP TO STOP"
                                            : "TAP FOR CLOCK";
  frame.setTextColor(t.accent, t.bg);
  frame.drawString(hint, CX, CY + 56, 2);

  // Center hub for the needle.
  frame.fillSmoothCircle(CX, CY + 0, 4, t.accent, t.bg);

  frame.pushSprite(0, 0);
}

// Decides what one tap does, based on which screen we're on.
static void handleTap() {
  switch (mode) {
    case MODE_CLOCK:
      // Cycle the dial color; after the last theme, enter the stopwatch.
      if (themeIdx + 1 < THEME_COUNT) {
        themeIdx++;
      } else {
        mode = SW_READY;  // stopwatch keeps the last theme's colors
      }
      break;
    case SW_READY:  // first tap on the stopwatch screen: start counting
      swStartMs = millis();
      mode = SW_RUNNING;
      break;
    case SW_RUNNING:  // second tap: freeze the time on screen
      swFrozenMs = millis() - swStartMs;
      mode = SW_STOPPED;
      break;
    case SW_STOPPED:  // third tap: reset and go back to the watch face
      swFrozenMs = 0;
      themeIdx = 0;  // restart the tap cycle from the first theme
      mode = MODE_CLOCK;
      break;
  }
}

// ============================ SETUP (runs once) =============================

void setup() {
  Serial.begin(115200);
  // The USB serial port needs a moment before the PC can see our messages.
  // Wait up to 4 seconds for it (but boot anyway if nothing connects).
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) delay(10);
  Serial.println("\n[boot] Round Display demo starting");

  // Turn the screen backlight on. (TFT_BL pin number comes from
  // platformio.ini; the library would do this too, but being explicit
  // makes "is the backlight even on?" easy to rule out when debugging.)
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  Serial.println("[boot] backlight pin driven HIGH (GPIO21)");

  // Touch interrupt pin idles HIGH and is pulled LOW during a touch.
  pinMode(TOUCH_INT_PIN, INPUT_PULLUP);
  Wire.begin();  // start I2C (D4 = SDA, D5 = SCL are the C3's defaults)

  // ---- Clock setup ----
  // The timezone must be set BEFORE parseBuildTime(), because the build
  // timestamp is local time and needs the TZ rule to convert correctly.
  setenv("TZ", TZ_INFO, 1);
  tzset();
  struct timeval tv = {parseBuildTime(), 0};
  settimeofday(&tv, nullptr);  // seed the system clock with build time

  // Ask the time-sync system to tell us (via serial) when NTP succeeds.
  sntp_set_time_sync_notification_cb([](struct timeval *) {
    Serial.println("[ntp] time synced from the internet");
  });

  // Start connecting to WiFi. This call returns immediately; the actual
  // connection happens in the background while the watch face runs.
  WiFi.mode(WIFI_STA);  // STA = "station", i.e. join a network as a client
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[wifi] connecting to \"%s\" in the background\n", WIFI_SSID);

  // ---- Screen setup ----
  tft.init();
  tft.setRotation(3);  // face upright with the USB cable at 6 o'clock

  // Visual self-test: flash red, green, blue so a display problem is obvious.
  Serial.println("[boot] tft.init() done, running color test");
  tft.fillScreen(TFT_RED);
  delay(600);
  tft.fillScreen(TFT_GREEN);
  delay(600);
  tft.fillScreen(TFT_BLUE);
  delay(600);
  tft.fillScreen(TFT_BLACK);
  Serial.println("[boot] color test done");

  // Create the off-screen frame buffer. At 16-bit color it needs
  // 240 x 240 x 2 bytes = ~115 KB of RAM. If that allocation ever fails,
  // drop to 8-bit color (~57 KB) instead of crashing.
  frame.setColorDepth(16);
  if (frame.createSprite(240, 240) == nullptr) {
    frame.setColorDepth(8);
    frame.createSprite(240, 240);
    Serial.println("[boot] 16-bit sprite failed, using 8-bit");
  }
  Serial.printf("[boot] setup complete, free heap: %u bytes\n",
                ESP.getFreeHeap());
}

// ======================= LOOP (runs over and over) ==========================

void loop() {
  // ---- One-time NTP kick-off, as soon as WiFi reports "connected" ----
  // configTzTime tells the ESP32 which servers to ask for the time; after
  // this, the system re-syncs itself automatically about once an hour.
  static bool ntpStarted = false;
  if (!ntpStarted && WiFi.status() == WL_CONNECTED) {
    ntpStarted = true;
    Serial.printf("[wifi] connected, IP %s\n",
                  WiFi.localIP().toString().c_str());
    configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.nist.gov");
  }

  // ---- Touch: each tap advances the theme/stopwatch cycle ----
  // The 350 ms check is a "debounce": one tap produces many touch readings
  // in a row, and without it one tap would count as several.
  static uint32_t lastTouchMs = 0;
  int16_t tx, ty;
  if (readTouch(tx, ty) && millis() - lastTouchMs > 350) {
    lastTouchMs = millis();
    handleTap();
    Serial.printf("Touch at (%d, %d) -> mode %u, theme %u\n",
                  tx, ty, mode, themeIdx);
  }

  // ---- Redraw the screen every 50 ms (about 20 frames per second) ----
  static uint32_t lastFrameMs = 0;
  if (millis() - lastFrameMs >= 50) {
    lastFrameMs = millis();
    if (mode == MODE_CLOCK) {
      drawClock();
    } else {
      drawStopwatch();
    }
  }
}
