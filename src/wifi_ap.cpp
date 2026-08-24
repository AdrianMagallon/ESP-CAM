#include "wifi_ap.h"
#include "Arduino.h"
#include "esp_wifi.h"
 
// How long to wait for the AP to assign itself an IP before giving up (ms)
#define AP_READY_TIMEOUT_MS 3000
 
// How often the watchdog checks the AP is still alive (ms)
#define WATCHDOG_CHECK_INTERVAL_MS 10000
 
// Stash these so the watchdog task can re-issue softAP() if it ever needs
// to bring the AP back up. Sized generously, doesn't need to be exact.
static char s_ssid[33]     = {};
static char s_password[65] = {};
 
static bool bringUpAP() {
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(s_ssid, s_password, /*channel=*/6);

  WiFi.softAPsetHostname("camera");
  if (!ok) {
    Serial.println("[wifi] softAP() call failed");
    return false;
  }
 
  uint32_t start = millis();
  while (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
    if (millis() - start > AP_READY_TIMEOUT_MS) {
      Serial.println("[wifi] Timed out waiting for AP IP");
      return false;
    }
    delay(100);
  }
 
  // Turn off power-save so the radio doesn't nap between packets — this is
  // what causes a lot of the stutter/lag on the MJPEG stream. Has to be set
  // after the AP is actually up, not before.
  esp_wifi_set_ps(WIFI_PS_NONE);
 
  Serial.printf("[wifi] AP ready — SSID: %s\n", s_ssid);
  Serial.printf("[wifi] Stream at: http://%s\n", WiFi.softAPIP().toString().c_str());
  return true;
}
 
// Just logs what's happening — doesn't do the actual recovery itself.
// The watchdog task handles recovery on its own schedule so we don't end
// up trying to restart the AP from inside an event callback.
static void onWifiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println("[wifi] AP started");
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      Serial.println("[wifi] AP stopped");
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.println("[wifi] Client connected");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.println("[wifi] Client disconnected");
      break;
    default:
      break; // don't care about the rest for now
  }
}
 
static void wifiWatchdogTask(void* param) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(WATCHDOG_CHECK_INTERVAL_MS));
 
    // Simplest possible health check: are we still in AP mode with a
    // valid IP assigned? If either of those is false, something died.
    bool modeOk = (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA);
    bool ipOk   = (WiFi.softAPIP() != IPAddress(0, 0, 0, 0));
 
    if (!modeOk || !ipOk) {
      Serial.println("[wifi] Watchdog: AP looks down, attempting restart...");
      WiFi.softAPdisconnect(true);
      delay(500);
 
      if (bringUpAP()) {
        Serial.println("[wifi] Watchdog: AP recovered");
      } else {
        Serial.println("[wifi] Watchdog: AP restart failed, will retry next cycle");
      }
    }
  }
  // never reached
}
 
bool startAP(const char* ssid, const char* password) {
  // Save these off so the watchdog can use them later without you having
  // to pass them around again.
  strlcpy(s_ssid, ssid, sizeof(s_ssid));
  strlcpy(s_password, password, sizeof(s_password));
 
  WiFi.onEvent(onWifiEvent);
 
  return bringUpAP();
}
 
void startWifiWatchdog() {
  // Small stack is fine — this task barely does anything but sleep and check.
  xTaskCreatePinnedToCore(
    wifiWatchdogTask,
    "wifi_watchdog",
    4096,
    nullptr,
    1,      // low priority, don't fight the camera/stream tasks for CPU
    nullptr,
    0       // pin to core 0, away from whatever's driving the camera loop
  );
  Serial.println("[wifi] Watchdog task started");
}