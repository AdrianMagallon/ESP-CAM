#include "local_ui.h"
#include "display.h"
#include "stream_server.h" // for getLatestFrameForDisplay()
#include "storage.h"        // for savePhoto/listPhotos/deletePhoto/getPhotoPath
#include "button.h"          // for buttonActivated() — physical TTP223 view toggle
#include <SD_MMC.h>
#include <string.h>
#include "esp_heap_caps.h"
#include <Arduino.h> // needed directly for int16_t/uint8_t and map() —
                     // don't rely on getting these secondhand through display.h

// ── Screen states ─────────────────────────────────────────────────────
// Mirrors the two-view structure of the web UI (camera-view /
// gallery-view in stream_server.cpp) — just native instead of HTML.
enum UIView { VIEW_CAMERA, VIEW_GALLERY };
static UIView s_view = VIEW_CAMERA;

// ── Button layout ────────────────────────────────────────────────────
struct UIButton {
  int16_t x, y, w, h;
  const char* label;
};

#define TOOLBAR_H 50
#define BTN_H     36

// Camera view buttons
static UIButton s_captureBtn;
static UIButton s_galleryBtn;

// Gallery view buttons
static UIButton s_backBtn;
static UIButton s_prevBtn;
static UIButton s_nextBtn;
static UIButton s_deleteBtn;

static bool pointInButton(int16_t px, int16_t py, const UIButton& b) {
  return px >= b.x && px <= (b.x + b.w) && py >= b.y && py <= (b.y + b.h);
}

// Positions everything relative to the actual panel size rather than
// hardcoding pixel values, so this stays correct if setRotation() in
// display.cpp ever changes. Buttons are spread evenly across the full
// width, with equal gaps on both sides and between buttons.
static void layoutButtons() {
  int16_t screenW = displayWidth();
  int16_t barY    = (int16_t)(displayHeight() - TOOLBAR_H + 7);
  int16_t btnW    = 90;

  // Camera view: 2 buttons
  {
    const int n   = 2;
    int16_t gap   = (screenW - n * btnW) / (n + 1);
    int16_t x     = gap;
    s_captureBtn = { x, barY, btnW, BTN_H, "Capture" }; x += btnW + gap;
    s_galleryBtn = { x, barY, btnW, BTN_H, "Gallery" };
  }

  // Gallery view: 4 buttons
  {
    const int n   = 4;
    int16_t gap   = (screenW - n * btnW) / (n + 1);
    int16_t x     = gap;
    s_backBtn   = { x, barY, btnW, BTN_H, "Back" };    x += btnW + gap;
    s_prevBtn   = { x, barY, btnW, BTN_H, "< Prev" };  x += btnW + gap;
    s_nextBtn   = { x, barY, btnW, BTN_H, "Next >" };  x += btnW + gap;
    s_deleteBtn = { x, barY, btnW, BTN_H, "Delete" };
  }
}

static void drawCameraToolbar() {
  displayDrawButton(s_captureBtn.x, s_captureBtn.y, s_captureBtn.w, s_captureBtn.h, s_captureBtn.label);
  displayDrawButton(s_galleryBtn.x, s_galleryBtn.y, s_galleryBtn.w, s_galleryBtn.h, s_galleryBtn.label);
}

static void drawGalleryToolbar() {
  displayDrawButton(s_backBtn.x, s_backBtn.y, s_backBtn.w, s_backBtn.h, s_backBtn.label);
  displayDrawButton(s_prevBtn.x, s_prevBtn.y, s_prevBtn.w, s_prevBtn.h, s_prevBtn.label);
  displayDrawButton(s_nextBtn.x, s_nextBtn.y, s_nextBtn.w, s_nextBtn.h, s_nextBtn.label);
  displayDrawButton(s_deleteBtn.x, s_deleteBtn.y, s_deleteBtn.w, s_deleteBtn.h, s_deleteBtn.label);
}

// ── Camera view state ────────────────────────────────────────────────
// Backlight has no PWM pin wired, so there's no real dimming control —
// always draw at full brightness.
#define CAMERA_REFRESH_MS 150
static uint32_t s_lastCameraDraw = 0;
static uint8_t* s_camBuf         = nullptr;
static size_t   s_camCap         = 0;

// ── Capture/delete feedback toast ────────────────────────────────────
#define TOAST_DURATION_MS       1500 // capture — camera view redraws over it naturally
#define TOAST_DURATION_FAST_MS  400  // delete — much faster, gallery has to clear it explicitly
static char     s_toastMsg[32] = "";
static uint32_t s_toastExpiry  = 0; // 0 = no toast on screen

// Draws immediately (so there's no lag waiting for the next scheduled
// camera refresh) and stays visible until s_toastExpiry. In camera view,
// the periodic refresh loop keeps redrawing it each frame until expiry,
// then clears the area once and carries on with plain frames. Gallery
// view has no such loop, so it explicitly checks s_toastExpiry and
// redraws the photo to clear it (see updateLocalUI).
static void showToast(const char* msg, uint32_t durationMs = TOAST_DURATION_MS) {
  strlcpy(s_toastMsg, msg, sizeof(s_toastMsg));
  s_toastExpiry = millis() + durationMs;
  if (s_toastExpiry == 0) s_toastExpiry = 1; // 0 is reserved for "no toast"

  displaySetViewport(0, 0, displayWidth(), displayHeight() - TOOLBAR_H);
  displayDrawToast(s_toastMsg);
  displayResetViewport();
}

// ── Gallery view state ───────────────────────────────────────────────
// The name list lives in PSRAM (allocated once in initLocalUI) — at 32
// bytes a name it's too big to leave sitting in internal RAM.
#define MAX_PHOTOS 1024
static char   (*s_photoNames)[PHOTO_NAME_MAX] = nullptr;
static int      s_photoCount     = 0;
static int      s_photoIndex     = 0;
static int      s_lastDrawnIndex = -1; // -1 forces a redraw on gallery entry
static uint8_t* s_photoBuf       = nullptr;
static size_t   s_photoCap       = 0;

// A toast the gallery should show right after its next photo redraw.
// Queued rather than drawn straight away because that redraw would wipe
// it out before it was ever visible.
static char     s_pendingToast[32]   = "";
static uint32_t s_pendingToastMs     = 0;

static void queueToast(const char* msg, uint32_t durationMs) {
  strlcpy(s_pendingToast, msg, sizeof(s_pendingToast));
  s_pendingToastMs = durationMs;
}

static bool collectPhotoName(const char* name, void* ctx) {
  if (s_photoCount < MAX_PHOTOS) {
    strlcpy(s_photoNames[s_photoCount], name, PHOTO_NAME_MAX);
    s_photoCount++;
  }
  return true; // keep going even when full, so we learn the real total
}

// Reads the photo names straight from storage into s_photoNames.
static void refreshPhotoList() {
  s_photoCount = 0;

  if (s_photoNames != nullptr) {
    int total = forEachPhoto(collectPhotoName, nullptr);
    if (total > s_photoCount) {
      Serial.printf("[local_ui] Gallery: %d photos on card, only showing the first %d\n",
                    total, s_photoCount);
      char msg[32];
      snprintf(msg, sizeof(msg), "Showing first %d of %d", s_photoCount, total);
      queueToast(msg, TOAST_DURATION_MS);
    }
  }

  if (s_photoIndex >= s_photoCount) {
    s_photoIndex = (s_photoCount > 0) ? s_photoCount - 1 : 0;
  }
  s_lastDrawnIndex = -1; // force a redraw with the fresh list
  Serial.printf("[local_ui] Gallery: %d photo(s)\n", s_photoCount);
}

// Reads a saved photo straight off the SD card — same file the web
// gallery's /photo?f=... endpoint serves, just read directly instead of
// round-tripping through HTTP.
static bool loadPhotoBytes(const char* filename, size_t* outLen) {
  char path[64];
  getPhotoPath(filename, path, sizeof(path));

  File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    Serial.printf("[local_ui] Failed to open %s\n", path);
    return false;
  }

  size_t len = f.size();
  if (len > s_photoCap) {
    uint8_t* bigger = (uint8_t*)realloc(s_photoBuf, len);
    if (bigger == nullptr) {
      f.close();
      Serial.println("[local_ui] Photo buffer realloc failed");
      return false;
    }
    s_photoBuf = bigger;
    s_photoCap = len;
  }

  size_t readLen = f.read(s_photoBuf, len);
  f.close();

  if (readLen != len) {
    Serial.printf("[local_ui] Short read on %s (%u/%u)\n", filename, (unsigned)readLen, (unsigned)len);
    return false;
  }

  *outLen = len;
  return true;
}

// Draws the current gallery photo in the area above the toolbar — or a
// line of text saying why there isn't one. Clipped to that area, so the
// toolbar never needs redrawing afterwards.
static void drawGalleryPhoto() {
  displaySetViewport(0, 0, displayWidth(), displayHeight() - TOOLBAR_H);
  displayClear();

  if (!storageAvailable()) {
    displayDrawMessage("No SD card");
  } else if (s_photoCount == 0) {
    displayDrawMessage("No photos yet");
  } else {
    size_t len = 0;
    if (loadPhotoBytes(s_photoNames[s_photoIndex], &len)) {
      displayDrawJpeg(s_photoBuf, len, 255);
    } else {
      Serial.printf("[local_ui] Failed to load %s\n", s_photoNames[s_photoIndex]);
      displayDrawMessage("Can't open photo");
    }
  }

  displayResetViewport();
}

// ── View transitions ─────────────────────────────────────────────────
static void enterCameraView() {
  s_view = VIEW_CAMERA;
  s_toastExpiry     = 0; // a toast from the other view doesn't carry over
  s_pendingToast[0] = '\0';
  displayClear();
  s_lastCameraDraw = 0; // force an immediate frame draw on the next update
  drawCameraToolbar();
}

static void enterGalleryView() {
  s_view = VIEW_GALLERY;
  s_toastExpiry     = 0;
  s_pendingToast[0] = '\0';
  displayClear();
  refreshPhotoList();
  drawGalleryToolbar();
}

// ── Public API ────────────────────────────────────────────────────────
void initLocalUI() {
  size_t listBytes = (size_t)MAX_PHOTOS * PHOTO_NAME_MAX;
  s_photoNames = (char (*)[PHOTO_NAME_MAX])heap_caps_malloc(listBytes, MALLOC_CAP_SPIRAM);
  if (s_photoNames == nullptr) {
    s_photoNames = (char (*)[PHOTO_NAME_MAX])malloc(listBytes); // no PSRAM — try internal RAM
  }
  if (s_photoNames == nullptr) {
    Serial.println("[local_ui] Couldn't allocate the gallery list — gallery will show as empty");
  }

  layoutButtons();
  enterCameraView(); // clears whatever boot text is still on screen
}

void updateLocalUI() {
  // Physical TTP223 button: 5 taps within ~2s (debounced in button.cpp)
  // toggles between camera and gallery, independent of the touchscreen.
  if (buttonActivated()) {
    Serial.println("[local_ui] Physical button: toggling view");
    if (s_view == VIEW_CAMERA) {
      enterGalleryView();
    } else {
      enterCameraView();
    }
  }

  int16_t x, y;
  bool tapped   = displayGetTouch(&x, &y);
  uint32_t now  = millis();

  if (s_view == VIEW_CAMERA) {
    if (now - s_lastCameraDraw > CAMERA_REFRESH_MS) {
      s_lastCameraDraw = now;
      size_t len = 0;
      if (getLatestFrameForDisplay(&s_camBuf, &s_camCap, &len) && len > 0) {
        // Clip drawing to the area above the toolbar so the camera image
        // can't paint over the buttons — means we no longer have to
        // redraw the toolbar every single refresh, just once on entry.
        displaySetViewport(0, 0, displayWidth(), displayHeight() - TOOLBAR_H);

        // Toast just ran out: wipe the area once. The camera image is
        // centred and may not cover every pixel the toast did, so a
        // plain frame on its own could leave a sliver of it behind.
        if (s_toastExpiry != 0 && now >= s_toastExpiry) {
          s_toastExpiry = 0;
          displayClear();
        }

        displayDrawJpeg(s_camBuf, len, 255);
        // Redraw the toast on top for as long as it's still active.
        if (s_toastExpiry != 0) {
          displayDrawToast(s_toastMsg);
        }
        displayResetViewport();
      }
    }

    if (tapped) {
      if (pointInButton(x, y, s_captureBtn)) {
        Serial.println("[local_ui] Capture button hit");
        char name[PHOTO_NAME_MAX] = {};
        size_t len = 0;
        // Same shared frame the web /capture endpoint reads from —
        // whatever's currently on screen is what gets saved.
        if (!storageAvailable()) {
          Serial.println("[local_ui] Capture: no SD card");
          showToast("No SD card");
        } else if (getLatestFrameForDisplay(&s_camBuf, &s_camCap, &len) && len > 0) {
          if (savePhoto(s_camBuf, len, name, sizeof(name))) {
            Serial.printf("[local_ui] Saved %s\n", name);
            showToast("Photo saved");
          } else {
            Serial.println("[local_ui] Capture: save failed");
            showToast("Save failed");
          }
        } else {
          Serial.println("[local_ui] Capture: no frame available yet");
          showToast("No frame yet");
        }
      } else if (pointInButton(x, y, s_galleryBtn)) {
        Serial.println("[local_ui] Gallery button hit");
        enterGalleryView();
      }
    }
    return;
  }

  // ── Gallery view ──────────────────────────────────────────────────
  if (tapped) {
    if (pointInButton(x, y, s_backBtn)) {
      enterCameraView();
      return;
    } else if (pointInButton(x, y, s_prevBtn)) {
      if (s_photoCount > 0) {
        s_photoIndex = (s_photoIndex == 0) ? (s_photoCount - 1) : (s_photoIndex - 1);
      }
    } else if (pointInButton(x, y, s_nextBtn)) {
      if (s_photoCount > 0) {
        s_photoIndex = (s_photoIndex + 1) % s_photoCount;
      }
    } else if (pointInButton(x, y, s_deleteBtn)) {
      if (s_photoCount > 0) {
        const char* name = s_photoNames[s_photoIndex];
        bool deleted = deletePhoto(name);
        if (deleted) {
          Serial.printf("[local_ui] Deleted %s\n", name);
        } else {
          Serial.printf("[local_ui] Delete failed: %s\n", name);
        }
        refreshPhotoList(); // re-fetch — indices shift after a delete
        queueToast(deleted ? "Deleted" : "Delete failed", TOAST_DURATION_FAST_MS);
      }
    }
  }

  // Only decode + draw when the photo actually changes — SD reads and
  // JPEG decode are slow, no reason to redo them every loop iteration.
  if (s_photoIndex != s_lastDrawnIndex) {
    s_lastDrawnIndex = s_photoIndex;
    s_toastExpiry    = 0; // the redraw wipes any toast that was showing
    drawGalleryPhoto();
  }

  // Show a queued toast AFTER the redraw above — drawing it any earlier
  // would've gotten it wiped out before it was ever visible.
  if (s_pendingToast[0] != '\0') {
    showToast(s_pendingToast, s_pendingToastMs);
    s_pendingToast[0] = '\0';
  }

  // Gallery has no periodic refresh loop like camera view does, so once
  // the toast's window is up, explicitly redraw the photo to wipe it —
  // otherwise it'd just sit on screen forever. Resetting s_toastExpiry
  // to 0 stops this from firing again every loop after it clears.
  if (s_toastExpiry != 0 && now >= s_toastExpiry) {
    s_toastExpiry = 0;
    drawGalleryPhoto();
  }
}
