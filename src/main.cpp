#include <WiFi.h>
#include "camera.h"
#include "wifi_ap.h"
#include "storage.h"
#include "stream_server.h"
#include "button.h"
#include "display.h"
#include "local_ui.h"
#include "user_setup.h"
#include <SD_MMC.h>

// AP credentials. To set your own without committing them, copy
// include/secrets.example.h to include/secrets.h (which git ignores) and
// edit it. The values below are only the fallback for when that file
// doesn't exist — don't ship them as-is.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "ESP32-S3-Cam"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "esp32stream"
#endif

#define BUTTON_PIN 14

// Stops here for good. The display comes up first in setup(), so the
// reason is on the screen as well as on serial — on a handheld there's
// usually no serial monitor attached to read it from.
static void haltWithError(const char* msg) {
  Serial.printf("[boot] %s — halting.\n", msg);
  displayClear();
  displayDrawMessage(msg);
  while (true) delay(1000);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[boot] Starting...");

  // Display first, so every later failure can be shown on it.
  if (!initDisplay()) {
    Serial.println("[boot] Display failed — halting.");
    while (true) delay(1000);
  }

  if (!initCamera()) {
    haltWithError("Camera failed");
  }

  // No SD card isn't fatal: the live view and the WiFi stream still
  // work, saving and the gallery just report "No SD card".
  if (!initStorage()) {
    Serial.println("[boot] Storage failed — carrying on without an SD card.");
    displayClear();
    displayDrawMessage("No SD card - photos disabled");
    delay(1500);
  }

  if (!startAP(WIFI_SSID, WIFI_PASS)) {
    haltWithError("WiFi AP failed");
  }

  startWifiWatchdog();

  if (!startServers()) {
    haltWithError("HTTP servers failed");
  }

  initButton(BUTTON_PIN);
  initLocalUI();

  Serial.println("[boot] All systems up.");
}

void loop() {
  updateLocalUI();

  static uint32_t lastLog = 0;
  if (millis() - lastLog > 10000) {
    lastLog = millis();
    Serial.printf("[loop] Connected clients: %d\n", WiFi.softAPgetStationNum());
  }

  // Touch only needs polling a couple of hundred times a second. Without
  // this the loop spins flat out and keeps a core busy for nothing.
  delay(5);
}
