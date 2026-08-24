#include "storage.h"
#include <SD_MMC.h>
#include "Arduino.h"

// ── SD card pin assignments for Freenove ESP32-S3 board ───────────
// 1-bit MMC mode — D1/D2/D3 are left alone (shared with camera)
#define SD_PIN_CLK  39
#define SD_PIN_CMD  38
#define SD_PIN_D0   40

// Photos stored flat in this directory — no subdirectories to avoid FAT quirks
#define PHOTO_DIR "/photos"

bool initStorage() {
  // Configure pins before mounting — 1-bit mode, no pull-ups (board has them)
  SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0);

  // true = 1-bit mode
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[storage] SD card mount failed — check card is inserted and pins are correct");
    return false;
  }

  // Log card type and size so you can confirm it's the right card
  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[storage] No SD card detected");
    return false;
  }

  Serial.printf("[storage] SD card mounted — %.1f GB total, %.1f GB free\n",
                SD_MMC.totalBytes() / 1e9,
                (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / 1e9);

  // Create photo directory if it doesn't exist yet
  if (!SD_MMC.exists(PHOTO_DIR)) {
    SD_MMC.mkdir(PHOTO_DIR);
    Serial.println("[storage] Created /photos directory");
  }

  return true;
}

void getPhotoPath(const char* name, char* pathOut, size_t pathLen) {
  snprintf(pathOut, pathLen, "%s/%s", PHOTO_DIR, name);
}

bool savePhoto(const uint8_t* data, size_t len, char* nameOut, size_t nameOutLen) {
  snprintf(nameOut, nameOutLen, "photo_%lu.jpg", millis());

  char path[64];
  getPhotoPath(nameOut, path, sizeof(path));

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("[storage] Failed to open %s for writing\n", path);
    return false;
  }

  f.write(data, len);
  f.close();

  Serial.printf("[storage] Saved %s (%u bytes)\n", nameOut, len);
  return true;
}

bool listPhotos(char* buf, size_t bufLen) {
  File dir = SD_MMC.open(PHOTO_DIR);
  if (!dir || !dir.isDirectory()) {
    strlcpy(buf, "[]", bufLen);
    return true;
  }

  size_t pos = 0;
  pos += snprintf(buf + pos, bufLen - pos, "[");
  bool first = true;

  File entry = dir.openNextFile();
  while (entry && pos < bufLen - 48) {
    if (!entry.isDirectory()) {
      const char* raw   = entry.name();
      const char* slash = strrchr(raw, '/');
      const char* name  = slash ? slash + 1 : raw;

      if (strncmp(name, "photo_", 6) == 0) {
        if (!first) pos += snprintf(buf + pos, bufLen - pos, ",");
        pos += snprintf(buf + pos, bufLen - pos, "\"%s\"", name);
        first = false;
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  snprintf(buf + pos, bufLen - pos, "]");
  return true;
}

bool deletePhoto(const char* name) {
  if (strncmp(name, "photo_", 6) != 0) {
    Serial.printf("[storage] Rejected delete of suspicious filename: %s\n", name);
    return false;
  }

  char path[64];
  getPhotoPath(name, path, sizeof(path));

  bool ok = SD_MMC.remove(path);
  Serial.printf("[storage] Delete %s: %s\n", name, ok ? "OK" : "FAIL");
  return ok;
}