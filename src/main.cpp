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

// AP credentials — change before flashing
#define WIFI_SSID "ESP32-S3-Cam"
#define WIFI_PASS "esp32stream"
#define BUTTON_PIN 14

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[boot] Starting...");

  if (!initCamera())  { 
    Serial.println("[boot] Camera failed — halting.");  
    while (true) delay(1000); 
    }

  if (!initStorage()) { 
    Serial.println("[boot] Storage failed — halting."); 
    while (true) delay(1000); 
    }

  if (!startAP(WIFI_SSID, WIFI_PASS)) { 
    Serial.println("[boot] WiFi AP failed — halting."); 
    while (true) delay(1000); 
    }

  startWifiWatchdog();

  if (!startServers()) { 
    Serial.println("[boot] HTTP servers failed — halting."); 
    while (true) delay(1000); 
    }

  if (!initDisplay()) {
    Serial.println("[boot] Display failed — halting.");
    while (true) delay(1000);
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
}