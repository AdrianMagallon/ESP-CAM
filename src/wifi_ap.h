
#pragma once
 
#include <WiFi.h>
#include <stdbool.h>
 
/**
 * Start a WiFi SoftAP with the given SSID and password.
 * Password must be at least 8 characters, or pass "" for open network.
 * Returns true on success, false if the AP failed to start.
 * Also disables WiFi power-save and registers event logging.
 */
bool startAP(const char* ssid, const char* password);
 
/**
 * Spin up a background FreeRTOS task that checks the AP is still alive
 * every few seconds and restarts it if it's dropped out. Call this once,
 * after startAP() has succeeded. Safe to call even if you never touch
 * WiFi again elsewhere in the sketch.
 */
void startWifiWatchdog();