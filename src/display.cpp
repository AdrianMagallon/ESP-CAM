#include "display.h"
#include <SPI.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <Preferences.h> // stores touch calibration in flash (NVS) so it
                          // only has to run once, not on every boot

#define TOUCH_COOLDOWN_MS 150

// TFT_eSPI reads its pins/driver from User_Setup.h, not from arguments
// here — that's just how this library works. All our pin numbers live
// in that config file now, not in this file.
static TFT_eSPI tft = TFT_eSPI();

static uint32_t s_lastTouchTime     = 0;
static uint8_t  s_activeBrightness  = 255; // read by the decoder callback below

// Scales one RGB565 pixel down toward black — our fake "brightness"
// control, since there's no PWM pin free for the real backlight.
static uint16_t scaleBrightness(uint16_t color565, uint8_t level) {
  if (level == 255) return color565;
  uint8_t r = (color565 >> 11) & 0x1F;
  uint8_t g = (color565 >> 5)  & 0x3F;
  uint8_t b =  color565        & 0x1F;
  r = (r * level) / 255;
  g = (g * level) / 255;
  b = (b * level) / 255;
  return (r << 11) | (g << 5) | b;
}

// TJpg_Decoder hands us one decoded block at a time through this callback.
static bool jpegOutputCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (s_activeBrightness != 255) {
    uint32_t pixelCount = (uint32_t)w * h;
    for (uint32_t i = 0; i < pixelCount; i++) {
      bitmap[i] = scaleBrightness(bitmap[i], s_activeBrightness);
    }
  }
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

static uint16_t s_calData[5]; // touch calibration data

bool initDisplay() {
  tft.init();
  tft.setRotation(1); // 0-3 — pick whichever makes the orientation right

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(20, 20);

  // getTouch() returns raw touch-controller coordinates, not screen
  // pixels — without calibration the two spaces don't line up and taps
  // never register on the buttons they're actually over. Only needs to
  // run once ever, though: the result gets saved to flash (NVS) below,
  // so every boot after the first just loads it back instead of asking
  // you to tap the corners again.
  //
  // DEBUG: flip this to true for one flash to wipe the stored calibration
  // and force a fresh interactive one — useful if touch stops working
  // after a "loaded from flash" boot and you want to rule out bad stored
  // data. Flip back to false afterward.
  const bool forceRecalibrate = false;

  Preferences prefs;
  prefs.begin("touchcal", false);

  if (forceRecalibrate) {
    prefs.remove("cal");
    Serial.println("[display] forceRecalibrate is on — cleared stored calibration");
  }

  bool haveStoredCal = (prefs.getBytesLength("cal") == sizeof(s_calData));
  if (haveStoredCal) {
    prefs.getBytes("cal", s_calData, sizeof(s_calData));
    tft.setTouch(s_calData);
    Serial.print("[display] Loaded touch calibration from flash: ");
    for (int i = 0; i < 5; i++) {
      Serial.print(s_calData[i]);
      Serial.print(i < 4 ? ", " : "\n");
    }
  } else {
    tft.println("Touch each corner...");
    tft.calibrateTouch(s_calData, TFT_MAGENTA, TFT_BLACK, 15);
    prefs.putBytes("cal", s_calData, sizeof(s_calData));
    Serial.print("[display] Touch calibration complete — saved to flash: ");
    for (int i = 0; i < 5; i++) {
      Serial.print(s_calData[i]);
      Serial.print(i < 4 ? ", " : "\n");
    }
  }

  prefs.end();

  tft.fillScreen(TFT_BLACK);
  tft.setCursor(20, 20);
  tft.println("Hello, camera!");

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(jpegOutputCallback);

  Serial.println("[display] Init done");
  return true;
}

uint16_t displayWidth()  { return tft.width(); }
uint16_t displayHeight() { return tft.height(); }

void displayClear() {
  tft.fillScreen(TFT_BLACK);
}

void displaySetViewport(int16_t x, int16_t y, int16_t w, int16_t h) {
  tft.setViewport(x, y, w, h);
}

void displayResetViewport() {
  tft.resetViewport();
}

void displayDrawButton(int16_t x, int16_t y, int16_t w, int16_t h, const char* label) {
  tft.fillRoundRect(x, y, w, h, 6, TFT_DARKGREY);
  tft.drawRoundRect(x, y, w, h, 6, TFT_WHITE);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(x + 8, y + (h / 2) - 4);
  tft.print(label);
}

void displayDrawToast(const char* msg) {
  // Dark green, no button-style border — visually distinct from
  // displayDrawButton so it doesn't invite a tap. Centered near the top
  // so it doesn't collide with the toolbar at the bottom.
  int16_t w = 220;
  int16_t h = 32;
  int16_t x = (tft.width() - w) / 2;
  int16_t y = 10;

  tft.fillRoundRect(x, y, w, h, 6, TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(x + 10, y + (h / 2) - 4);
  tft.print(msg);
}

static bool s_wasTouched = false; // tracks press state across calls, for edge detection

bool displayGetTouch(int16_t* outX, int16_t* outY) {
  uint16_t x, y;
  bool isTouchedNow = tft.getTouch(&x, &y);

  if (!isTouchedNow) {
    s_wasTouched = false; // released — next press is free to register
    return false;
  }

  // Only fire on the transition from not-touched to touched. Without
  // this, holding a finger down fires again every time the cooldown
  // below expires, for as long as the finger stays put — one physical
  // tap turning into several registered taps.
  if (s_wasTouched) {
    return false;
  }

  uint32_t now = millis();
  if (now - s_lastTouchTime < TOUCH_COOLDOWN_MS) {
    return false; // debounces contact noise right at the moment of touchdown
  }

  s_wasTouched    = true;
  s_lastTouchTime = now;
  *outX = x;
  *outY = y;
  return true;
}

void displayDrawJpeg(const uint8_t* jpegBuf, size_t len, uint8_t brightness) {
  s_activeBrightness = brightness;
  TJpgDec.drawJpg(0, 0, jpegBuf, len);
}