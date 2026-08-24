#include "local_ui.h"
#include "display.h"
#include "stream_server.h" // for getLatestFrameForDisplay()
#include "storage.h"        // for savePhoto/listPhotos/deletePhoto/getPhotoPath
#include "button.h"          // for buttonActivated() — physical TTP223 view toggle
#include <SD_MMC.h>
#include <string.h>
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
static uint32_t s_toastExpiry  = 0;

// Draws immediately (so there's no lag waiting for the next scheduled
// camera refresh) and stays visible until s_toastExpiry. In camera view,
// the periodic refresh loop keeps redrawing it each frame until expiry,
// then a normal camera frame draws through without it — no extra code
// needed there. Gallery view has no such loop, so it explicitly checks
// s_toastExpiry and redraws the photo to clear it (see updateLocalUI).
static void showToast(const char* msg, uint32_t durationMs = TOAST_DURATION_MS) {
  strlcpy(s_toastMsg, msg, sizeof(s_toastMsg));
  s_toastExpiry = millis() + durationMs;

  displaySetViewport(0, 0, displayWidth(), displayHeight() - TOOLBAR_H);
  displayDrawToast(s_toastMsg);
  displayResetViewport();
}

// ── Gallery view state ───────────────────────────────────────────────
#define MAX_PHOTOS     64
#define PHOTO_NAME_LEN 32
static char     s_photoNames[MAX_PHOTOS][PHOTO_NAME_LEN];
static int      s_photoCount     = 0;
static int      s_photoIndex     = 0;
static int      s_lastDrawnIndex = -1; // -1 forces a redraw on gallery entry
static uint8_t* s_photoBuf       = nullptr;
static size_t   s_photoCap       = 0;

// Parses the JSON array listPhotos() returns (e.g.
// ["photo_1.jpg","photo_2.jpg"]) into s_photoNames. Simple quote-scanning
// is fine here since our filenames are machine-generated in storage.cpp
// and never contain a stray '"'.
static void refreshPhotoList() {
  char* buf = (char*)malloc(4096);
  if (buf == nullptr) {
    Serial.println("[local_ui] refreshPhotoList: out of memory");
    return;
  }

  listPhotos(buf, 4096);

  s_photoCount = 0;
  char* p = buf;
  while (*p && s_photoCount < MAX_PHOTOS) {
    char* start = strchr(p, '"');
    if (start == nullptr) break;
    start++;
    char* end = strchr(start, '"');
    if (end == nullptr) break;

    size_t len = end - start;
    if (len >= PHOTO_NAME_LEN) len = PHOTO_NAME_LEN - 1;
    memcpy(s_photoNames[s_photoCount], start, len);
    s_photoNames[s_photoCount][len] = '\0';
    s_photoCount++;

    p = end + 1;
  }

  free(buf);

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

// ── View transitions ─────────────────────────────────────────────────
static void enterCameraView() {
  s_view = VIEW_CAMERA;
  displayClear();
  s_lastCameraDraw = 0; // force an immediate frame draw on the next update
  drawCameraToolbar();
}

static void enterGalleryView() {
  s_view = VIEW_GALLERY;
  displayClear();
  refreshPhotoList();
  drawGalleryToolbar();
}

// ── Public API ────────────────────────────────────────────────────────
void initLocalUI() {
  layoutButtons();
  drawCameraToolbar();
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
        displayDrawJpeg(s_camBuf, len, 255);
        // Redraw the toast on top for as long as it's still active —
        // once s_toastExpiry passes, a plain camera frame draws through
        // and the toast disappears on its own, no manual clear needed.
        if (now < s_toastExpiry) {
          displayDrawToast(s_toastMsg);
        }
        displayResetViewport();
      }
    }

    if (tapped) {
      if (pointInButton(x, y, s_captureBtn)) {
        Serial.println("[local_ui] Capture button hit");
        char name[48] = {};
        size_t len = 0;
        // Same shared frame the web /capture endpoint reads from —
        // whatever's currently on screen is what gets saved.
        if (getLatestFrameForDisplay(&s_camBuf, &s_camCap, &len) && len > 0) {
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
  bool justDeleted  = false;
  bool deleteFailed = false;

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
        if (deletePhoto(name)) {
          Serial.printf("[local_ui] Deleted %s\n", name);
          justDeleted = true;
        } else {
          Serial.printf("[local_ui] Delete failed: %s\n", name);
          deleteFailed = true;
        }
        refreshPhotoList(); // re-fetch — indices shift after a delete
      }
    }
  }

  // Only decode + draw when the photo actually changes — SD reads and
  // JPEG decode are slow, no reason to redo them every loop iteration.
  if (s_photoIndex != s_lastDrawnIndex) {
    s_lastDrawnIndex = s_photoIndex;
    displayClear();

    if (s_photoCount == 0) {
      Serial.println("[local_ui] Gallery is empty");
    } else {
      size_t len = 0;
      if (loadPhotoBytes(s_photoNames[s_photoIndex], &len)) {
        displayDrawJpeg(s_photoBuf, len, 255);
      } else {
        Serial.printf("[local_ui] Failed to load %s\n", s_photoNames[s_photoIndex]);
      }
    }

    drawGalleryToolbar();
  }

  // Draw the delete toast AFTER the redraw above, not inside the tap
  // handler — refreshPhotoList() just forced a fresh photo draw this
  // same tick, and drawing the toast beforehand would've gotten wiped
  // out before it was ever visible.
  if (justDeleted) {
    showToast("Deleted", TOAST_DURATION_FAST_MS);
  } else if (deleteFailed) {
    showToast("Delete failed", TOAST_DURATION_FAST_MS);
  }

  // Gallery has no periodic refresh loop like camera view does, so once
  // the toast's short window is up, explicitly redraw the photo to wipe
  // it — otherwise it'd just sit on screen forever. The s_toastExpiry
  // reset to 0 stops this from firing again every loop after it clears.
  if (s_toastExpiry != 0 && now >= s_toastExpiry) {
    s_toastExpiry = 0;
    displayClear();
    if (s_photoCount > 0) {
      size_t len = 0;
      if (loadPhotoBytes(s_photoNames[s_photoIndex], &len)) {
        displayDrawJpeg(s_photoBuf, len, 255);
      }
    }
    drawGalleryToolbar();
  }
}