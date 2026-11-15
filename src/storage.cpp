#include "storage.h"
#include <SD_MMC.h>
#include <atomic>
#include "Arduino.h"

// ── SD card pin assignments for Freenove ESP32-S3 board ───────────
// 1-bit MMC mode — D1/D2/D3 are left alone (shared with camera)
#define SD_PIN_CLK  39
#define SD_PIN_CMD  38
#define SD_PIN_D0   40

// Photos stored flat in this directory — no subdirectories to avoid FAT quirks
#define PHOTO_DIR    "/photos"
#define PHOTO_PREFIX "photo_"

static bool s_mounted = false;

// Number the next saved photo gets. Atomic because savePhoto() is called
// from two tasks: the HTTP server (web capture) and loop() (touchscreen
// capture).
static std::atomic<uint32_t> s_nextIndex{1};

static const char* baseName(const char* raw) {
  const char* slash = strrchr(raw, '/');
  return slash ? slash + 1 : raw;
}

bool storageAvailable() {
  return s_mounted;
}

bool isValidPhotoName(const char* name) {
  if (name == nullptr) return false;

  size_t len = strlen(name);
  if (len >= PHOTO_NAME_MAX) return false;
  if (strncmp(name, PHOTO_PREFIX, strlen(PHOTO_PREFIX)) != 0) return false;

  // No path separators and no ".." — a name can only ever point at a
  // file sitting directly inside PHOTO_DIR.
  if (strchr(name, '/') != nullptr || strchr(name, '\\') != nullptr) return false;
  if (strstr(name, "..") != nullptr) return false;

  return true;
}

// Finds the highest photo number already on the card so new photos carry
// on from there instead of starting over (and overwriting) after a reboot.
static bool findHighestIndex(const char* name, void* ctx) {
  uint32_t* highest = static_cast<uint32_t*>(ctx);
  uint32_t n = strtoul(name + strlen(PHOTO_PREFIX), nullptr, 10);
  if (n > *highest) *highest = n;
  return true;
}

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
    if (!SD_MMC.mkdir(PHOTO_DIR)) {
      Serial.println("[storage] Failed to create /photos directory");
      return false;
    }
    Serial.println("[storage] Created /photos directory");
  }

  s_mounted = true;

  uint32_t highest = 0;
  int count = forEachPhoto(findHighestIndex, &highest);
  // highest + 1 would wrap to 0 only if a file is literally numbered
  // UINT32_MAX — not worth more than falling back to 1.
  s_nextIndex = (highest == UINT32_MAX) ? 1 : highest + 1;
  Serial.printf("[storage] %d photo(s) on card, next is photo_%lu.jpg\n",
                count, (unsigned long)s_nextIndex.load());

  return true;
}

void getPhotoPath(const char* name, char* pathOut, size_t pathLen) {
  snprintf(pathOut, pathLen, "%s/%s", PHOTO_DIR, name);
}

bool savePhoto(const uint8_t* data, size_t len, char* nameOut, size_t nameOutLen) {
  if (!s_mounted) {
    Serial.println("[storage] Save skipped — no SD card");
    return false;
  }

  char path[64];

  // Take the next number. The exists() check is belt and braces — the
  // counter already starts past everything on the card, so this only
  // loops if a file appeared some other way.
  do {
    uint32_t index = s_nextIndex.fetch_add(1);
    snprintf(nameOut, nameOutLen, PHOTO_PREFIX "%lu.jpg", (unsigned long)index);
    getPhotoPath(nameOut, path, sizeof(path));
  } while (SD_MMC.exists(path));

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("[storage] Failed to open %s for writing\n", path);
    return false;
  }

  size_t written = f.write(data, len);
  f.close();

  if (written != len) {
    // Card full or write error. A half-written JPEG is worse than no
    // file at all, so don't leave it lying around in the gallery.
    Serial.printf("[storage] Short write on %s (%u/%u bytes) — removing it\n",
                  nameOut, (unsigned)written, (unsigned)len);
    SD_MMC.remove(path);
    return false;
  }

  Serial.printf("[storage] Saved %s (%u bytes)\n", nameOut, (unsigned)len);
  return true;
}

int forEachPhoto(PhotoVisitor cb, void* ctx) {
  if (!s_mounted) return 0;

  File dir = SD_MMC.open(PHOTO_DIR);
  if (!dir || !dir.isDirectory()) {
    return 0;
  }

  int count = 0;
  bool keepGoing = true;

  File entry = dir.openNextFile();
  while (entry && keepGoing) {
    if (!entry.isDirectory()) {
      const char* name = baseName(entry.name());
      if (isValidPhotoName(name)) {
        count++;
        keepGoing = cb(name, ctx);
      }
    }
    entry.close();
    if (keepGoing) entry = dir.openNextFile();
  }
  dir.close();

  return count;
}

bool deletePhoto(const char* name) {
  if (!s_mounted) return false;

  if (!isValidPhotoName(name)) {
    Serial.println("[storage] Rejected delete of suspicious filename");
    return false;
  }

  char path[64];
  getPhotoPath(name, path, sizeof(path));

  bool ok = SD_MMC.remove(path);
  Serial.printf("[storage] Delete %s: %s\n", name, ok ? "OK" : "FAIL");
  return ok;
}
