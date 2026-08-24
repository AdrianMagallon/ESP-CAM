#include "stream_server.h"
#include "storage.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <SD_MMC.h>
#include "Arduino.h"

// ── MJPEG multipart stream constants ─────────────────────────────────────
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART_HEADER  = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Two separate server instances, each with its own worker task — the
// stream handler below runs forever for as long as a client is watching,
// so it needs its own dedicated worker or it'll block every other request.
static httpd_handle_t s_api_httpd    = nullptr;  // port 80
static httpd_handle_t s_stream_httpd = nullptr;  // port 81

// ── UI — single-page app with camera view and photo gallery ──────────────
static const char* ROOT_HTML = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
  <title>Camera</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; -webkit-tap-highlight-color: transparent; }

    body {
      font-family: -apple-system, sans-serif;
      background: #000;
      overflow: hidden;
      height: 100dvh;
    }

    /* ── Views ─────────────────────────────────────────────── */
    .view { display: none; width: 100%; flex-direction: column; }
    .view.active { display: flex; height: 100dvh; }

    /* ── Camera view ───────────────────────────────────────── */
    #camera-view { background: #000; }

    #stream-wrapper {
      flex: 1;
      position: relative;
      overflow: hidden;
      background: #000;
    }

    #stream {
      width: 100%;
      height: 100%;
      object-fit: cover;
      display: block;
    }

    /* In portrait, rotate the landscape stream to fill the screen */
    @media (orientation: portrait) {
      #stream {
        position: absolute;
        top: 50%;
        left: 50%;
        width: calc(100dvh - 90px); /* wrapper height becomes stream width after rotation */
        height: 100dvw;             /* wrapper width becomes stream height after rotation */
        transform: translate(-50%, -50%) rotate(90deg);
        object-fit: cover;
      }
    }

    /* White flash on shutter press */
    #flash {
      position: absolute;
      inset: 0;
      background: #fff;
      opacity: 0;
      pointer-events: none;
      z-index: 5;
      transition: opacity 0.05s;
    }
    #flash.active { opacity: 1; }

    /* ── Shared toolbar ────────────────────────────────────── */
    .toolbar {
      height: 90px;
      background: #fff;
      display: flex;
      align-items: center;
      padding: 0 32px;
      gap: 12px;
      padding-bottom: env(safe-area-inset-bottom, 0);
      flex-shrink: 0;
    }

    .toolbar-spacer { flex: 1; }

    /* Pill-shaped icon buttons */
    .pill-btn {
      background: #e5e5ea;
      border: none;
      border-radius: 50px;
      width: 72px;
      height: 50px;
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: pointer;
      transition: opacity 0.15s;
      flex-shrink: 0;
    }
    .pill-btn:active { opacity: 0.45; }
    .pill-btn svg { width: 22px; height: 22px; }

    /* ── Gallery view ──────────────────────────────────────── */
    #gallery-view { background: #e5e5ea; }

    #photo-grid {
      flex: 1;
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: 3px;
      padding: 3px;
      overflow-y: auto;
      align-content: start;
    }

    .photo-cell {
      aspect-ratio: 4 / 3;
      background: #c7c7cc;
      border-radius: 4px;
      overflow: hidden;
      position: relative;
      cursor: pointer;
    }

    .photo-cell img {
      width: 100%;
      height: 100%;
      object-fit: cover;
      display: block;
    }

    /* Selection ring in top-right corner of each cell */
    .sel-ring {
      position: absolute;
      top: 5px;
      right: 5px;
      width: 20px;
      height: 20px;
      border-radius: 50%;
      border: 2px solid rgba(255,255,255,0.85);
      background: transparent;
      pointer-events: none;
      transition: background 0.1s;
    }

    /* Filled dot when selected */
    .photo-cell.selected .sel-ring {
      background: #fff;
      border-color: #fff;
    }
    .photo-cell.selected .sel-ring::after {
      content: '';
      position: absolute;
      inset: 3px;
      border-radius: 50%;
      background: #1c1c1e;
    }

    .empty-state {
      grid-column: 1 / -1;
      display: flex;
      height: 200px;
      align-items: center;
      justify-content: center;
      color: #8e8e93;
      font-size: 14px;
    }

    /* ── Toast notification ────────────────────────────────── */
    #toast {
      position: fixed;
      bottom: 110px;
      left: 50%;
      transform: translateX(-50%);
      background: rgba(0,0,0,0.72);
      color: #fff;
      padding: 8px 18px;
      border-radius: 20px;
      font-size: 13px;
      pointer-events: none;
      opacity: 0;
      transition: opacity 0.25s;
      white-space: nowrap;
      z-index: 200;
    }
    #toast.show { opacity: 1; }
  </style>
</head>
<body>

<!-- ── Camera view ──────────────────────────────────────────────── -->
<div id="camera-view" class="view active">
  <div id="stream-wrapper">
    <img id="stream" alt="">
    <div id="flash"></div>
  </div>
  <div class="toolbar">
    <div class="toolbar-spacer"></div>
    <button class="pill-btn" onclick="capturePhoto()" title="Take photo">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
        <path d="M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z"/>
        <circle cx="12" cy="13" r="4"/>
      </svg>
    </button>
    <div class="toolbar-spacer"></div>
    <button class="pill-btn" onclick="showGallery()" title="Gallery">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
        <rect x="3" y="3" width="18" height="18" rx="2"/>
        <circle cx="8.5" cy="8.5" r="1.5"/>
        <polyline points="21 15 16 10 5 21"/>
      </svg>
    </button>
    <div class="toolbar-spacer"></div>
  </div>
</div>

<!-- ── Gallery view ─────────────────────────────────────────────── -->
<div id="gallery-view" class="view">
  <div id="photo-grid"></div>
  <div class="toolbar">
    <button class="pill-btn" onclick="showCamera()" title="Back">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <polyline points="15 18 9 12 15 6"/>
      </svg>
    </button>
    <div class="toolbar-spacer"></div>
    <button class="pill-btn" onclick="downloadSelected()" title="Download selected">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
        <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/>
        <polyline points="7 10 12 15 17 10"/>
        <line x1="12" y1="15" x2="12" y2="3"/>
      </svg>
    </button>
    <button class="pill-btn" onclick="deleteSelected()" title="Delete selected">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
        <polyline points="3 6 5 6 21 6"/>
        <path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/>
        <path d="M10 11v6M14 11v6"/>
        <path d="M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/>
      </svg>
    </button>
  </div>
</div>

<div id="toast"></div>

<script>
  // Stream lives on its own server (port 81), with its own dedicated
  // worker task, so the never-ending stream loop can't block requests
  // like /capture or /photo from being handled on the API server.
  const STREAM_URL = 'http://' + location.hostname + ':81/stream';
  const streamImg  = document.getElementById('stream');
  const selected   = new Set();
  let toastTimer   = null;

  // Start stream on load
  streamImg.src = STREAM_URL;

  function showGallery() {
    streamImg.src = '';  // pause stream — frees bandwidth and camera buffers
    document.getElementById('camera-view').classList.remove('active');
    document.getElementById('gallery-view').classList.add('active');
    selected.clear();
    loadPhotos();
  }

  function showCamera() {
    document.getElementById('gallery-view').classList.remove('active');
    document.getElementById('camera-view').classList.add('active');
    selected.clear();
    streamImg.src = STREAM_URL;  // resume stream
  }

  async function capturePhoto() {
    // Visual shutter flash
    const flash = document.getElementById('flash');
    flash.classList.add('active');
    setTimeout(() => flash.classList.remove('active'), 80);

    try {
      const res = await fetch('/capture');
      if (!res.ok) throw new Error('HTTP ' + res.status);
      showToast('Photo saved');
    } catch (e) {
      showToast('Capture failed');
    }
  }

  async function loadPhotos() {
    const grid = document.getElementById('photo-grid');
    grid.innerHTML = '';
    try {
      const files = await fetch('/photos').then(r => r.json());
      if (files.length === 0) {
        grid.innerHTML = '<div class="empty-state">No photos yet</div>';
        return;
      }
      for (const f of files) {
        const cell = document.createElement('div');
        cell.className = 'photo-cell';
        cell.innerHTML = `<img src="/photo?f=${encodeURIComponent(f)}" loading="lazy">
                          <div class="sel-ring"></div>`;
        cell.addEventListener('click', () => toggleSelect(f, cell));
        grid.appendChild(cell);
      }
    } catch {
      grid.innerHTML = '<div class="empty-state">Failed to load photos</div>';
    }
  }

  function toggleSelect(filename, cell) {
    if (selected.has(filename)) {
      selected.delete(filename);
      cell.classList.remove('selected');
    } else {
      selected.add(filename);
      cell.classList.add('selected');
    }
  }

  async function downloadSelected() {
    if (selected.size === 0) { showToast('Select photos first'); return; }
    let count = 0;
    for (const f of selected) {
      try {
        const blob = await fetch('/photo?f=' + encodeURIComponent(f)).then(r => r.blob());
        const url  = URL.createObjectURL(blob);
        const a    = Object.assign(document.createElement('a'), { href: url, download: f });
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
        // Safari needs a moment between download triggers
        await new Promise(r => setTimeout(r, 400));
        count++;
      } catch {
        showToast('Download failed'); return;
      }
    }
    showToast('Downloaded ' + count + (count === 1 ? ' photo' : ' photos'));
  }

  async function deleteSelected() {
    if (selected.size === 0) { showToast('Select photos first'); return; }
    const count = selected.size;
    for (const f of selected) {
      await fetch('/delete?f=' + encodeURIComponent(f)).catch(() => {});
    }
    selected.clear();
    showToast('Deleted ' + count + (count === 1 ? ' photo' : ' photos'));
    loadPhotos();
  }

  function showToast(msg) {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.remove('show'), 2000);
  }
</script>
</body>
</html>
)HTML";

// ─────────────────────────────────────────────────────────────────────────
// Handlers
// ─────────────────────────────────────────────────────────────────────────

// ── Shared "latest frame" buffer ──────────────────────────────────────────
// A dedicated task keeps this filled with whatever the camera just captured.
// Both the stream handler and the /capture endpoint read from here instead
// of calling esp_camera_fb_get() themselves — only the capture task should
// ever talk to the camera driver directly, since it's not built to have
// two tasks pulling frames from it at once.
static SemaphoreHandle_t s_frame_mutex   = nullptr;
static uint8_t*          s_frame_buf     = nullptr; // grows as needed, lives in PSRAM
static size_t             s_frame_cap     = 0;
static size_t             s_frame_len     = 0;
static volatile bool      s_frame_ready   = false;

// Runs on core 1, away from the WiFi/network stack on core 0. Its only job
// is "grab a frame, copy it into the shared buffer, repeat" — it never
// waits on the network, so a slow WiFi send can no longer stall the camera.
static void captureTask(void* param) {
  for (;;) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
      Serial.println("[capture] Frame grab failed");
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (fb->len > s_frame_cap) {
        // heap_caps_realloc keeps this in PSRAM rather than falling back
        // to the much smaller internal RAM
        uint8_t* bigger = (uint8_t*)heap_caps_realloc(s_frame_buf, fb->len, MALLOC_CAP_SPIRAM);
        if (bigger != nullptr) {
          s_frame_buf = bigger;
          s_frame_cap = fb->len;
        }
      }
      if (s_frame_buf != nullptr && fb->len <= s_frame_cap) {
        memcpy(s_frame_buf, fb->buf, fb->len);
        s_frame_len   = fb->len;
        s_frame_ready = true;
      }
      xSemaphoreGive(s_frame_mutex);
    }

    esp_camera_fb_return(fb);
    // No delay here on purpose — let it run as fast as the camera can
    // produce frames. grab_mode LATEST (set in camera.cpp) means we're
    // never chewing through a backlog of stale frames.
  }
}

// Copies whatever the newest frame is into the caller's own buffer, growing
// it if needed. Keeping this copy local to the caller means the mutex only
// has to be held for a fast memcpy, not for the entire network send.
bool getLatestFrameForDisplay(uint8_t** buf, size_t* cap, size_t* outLen) {
  bool ok = false;
  if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    if (s_frame_ready && s_frame_len > 0) {
      if (s_frame_len > *cap) {
        uint8_t* bigger = (uint8_t*)realloc(*buf, s_frame_len);
        if (bigger != nullptr) { *buf = bigger; *cap = s_frame_len; }
      }
      if (*buf != nullptr && s_frame_len <= *cap) {
        memcpy(*buf, s_frame_buf, s_frame_len);
        *outLen = s_frame_len;
        ok = true;
      }
    }
    xSemaphoreGive(s_frame_mutex);
  }
  return ok;
}

// Call once, before startServers(). Sets up the shared buffer and gets the
// capture task running so there's a frame ready by the time anyone asks.
static bool startCapturePipeline() {
  s_frame_mutex = xSemaphoreCreateMutex();
  if (s_frame_mutex == nullptr) {
    Serial.println("[capture] Failed to create frame mutex");
    return false;
  }

  // Pinned to core 1 — the WiFi driver and our httpd servers live on core 0
  // (see the core_id settings in startServers() below), so this keeps
  // camera work and network work fully off each other's toes.
  BaseType_t ok = xTaskCreatePinnedToCore(
    captureTask,
    "cam_capture",
    4096,
    nullptr,
    2,      // a bit above the wifi watchdog's priority — this is the hot path
    nullptr,
    1       // core 1
  );

  if (ok != pdPASS) {
    Serial.println("[capture] Failed to start capture task");
    return false;
  }

  Serial.println("[capture] Capture task started on core 1");
  return true;
}


// GET / — serves the UI page
static esp_err_t root_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, ROOT_HTML, strlen(ROOT_HTML));
}

// GET /stream — continuous MJPEG stream (runs on port 81 server)
static esp_err_t stream_handler(httpd_req_t* req) {
  esp_err_t res = ESP_OK;
  char part_buf[128];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  // Allow the page (served on port 80) to load this cross-origin stream
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  // Local copy of whatever the latest frame is — kept separate from the
  // shared buffer so the mutex is only held for the memcpy, not for the
  // (much slower, network-speed-dependent) send below.
  uint8_t* local_buf = nullptr;
  size_t   local_cap = 0;

  while (true) {
    size_t len = 0;
    if (!getLatestFrameForDisplay(&local_buf, &local_cap, &len) || len == 0) {
      // Capture task hasn't produced a frame yet, or the mutex was busy —
      // wait a beat and try again rather than spinning.
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res != ESP_OK) break;

    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART_HEADER, len);
    res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res != ESP_OK) break;

    res = httpd_resp_send_chunk(req, (const char*)local_buf, len);
    if (res != ESP_OK) break; // client disconnected
  }

  free(local_buf);
  Serial.println("[stream] Client disconnected");
  return res;
}

// GET /capture — grab a frame and save it to flash
static esp_err_t capture_handler(httpd_req_t* req) {
  static uint8_t* capture_buf = nullptr;
  static size_t   capture_cap = 0;
  size_t len = 0;

  if (!getLatestFrameForDisplay(&capture_buf, &capture_cap, &len) || len == 0) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No frame available yet");
    return ESP_FAIL;
  }

  char name[48] = {};
  bool ok = savePhoto(capture_buf, len, name, sizeof(name));

  if (!ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save to flash failed");
    return ESP_FAIL;
  }

  char resp[72];
  snprintf(resp, sizeof(resp), "{\"file\":\"%s\"}", name);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, resp);
}

// GET /photos — return JSON array of saved photo filenames
static esp_err_t photos_handler(httpd_req_t* req) {
  // Heap-allocate — avoid blowing the HTTP server task stack
  char* buf = static_cast<char*>(malloc(4096));
  if (buf == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }

  listPhotos(buf, 4096);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  esp_err_t res = httpd_resp_sendstr(req, buf);
  free(buf);
  return res;
}

// GET /photo?f=<filename> — serve a saved JPEG from flash
static esp_err_t photo_handler(httpd_req_t* req) {
  char query[96]    = {};
  char filename[48] = {};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "f", filename, sizeof(filename)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ?f=filename");
    return ESP_FAIL;
  }

  // Reject anything that doesn't look like one of our files
  if (strncmp(filename, "photo_", 6) != 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
    return ESP_FAIL;
  }

  char path[64];
  getPhotoPath(filename, path, sizeof(path));

File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Photo not found");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
  httpd_resp_set_hdr(req, "Connection", "close"); // free the socket right after, don't hold it on keep-alive

  uint8_t buf[512];
  esp_err_t res = ESP_OK;
  while (f.available() && res == ESP_OK) {
    size_t n = f.read(buf, sizeof(buf));
    if (n > 0) res = httpd_resp_send_chunk(req, (const char*)buf, n);
  }
  f.close();

  httpd_resp_send_chunk(req, nullptr, 0); // end chunked transfer
  return res;
}

// GET /delete?f=<filename> — delete a saved photo from flash
static esp_err_t delete_handler(httpd_req_t* req) {
  char query[96]    = {};
  char filename[48] = {};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "f", filename, sizeof(filename)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ?f=filename");
    return ESP_FAIL;
  }

  if (!deletePhoto(filename)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
    return ESP_FAIL;
  }

  return httpd_resp_sendstr(req, "OK");
}

// ── Helpers ───────────────────────────────────────────────────────────────

static esp_err_t register_route(httpd_handle_t server, const char* uri,
                                 httpd_method_t method,
                                 esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t route = { .uri=uri, .method=method, .handler=handler, .user_ctx=nullptr };
  return httpd_register_uri_handler(server, &route);
}

// ─────────────────────────────────────────────────────────────────────────

bool startServers() {
  // Camera capture now happens on its own task on core 1 — see
  // startCapturePipeline() above. Has to be up before either server
  // starts handling requests, since /stream and /capture both read from
  // the buffer it fills.
  if (!startCapturePipeline()) {
    return false;
  }

  // ── API server on port 80 ───────────────────────────────────────────────
  // Handles the UI page, capture, gallery listing, photo downloads, delete.
  {
    delay(500); // give the OS a moment to release the socket from last boot
    httpd_config_t cfg  = HTTPD_DEFAULT_CONFIG();
    cfg.server_port     = 80;
    cfg.ctrl_port       = 32768;
    // Keeping this modest — each server costs roughly (max_open_sockets + 2)
    // for its listener + internal control socket, and the chip has 10
    // sockets total system-wide, shared with the stream server below AND
    // whatever the WiFi/DHCP internals reserve for themselves. Going too
    // high here is what caused the stream server to fail to start last
    // time, so leaving real headroom this time rather than cutting it close.
    cfg.max_open_sockets = 4;

    if (httpd_start(&s_api_httpd, &cfg) != ESP_OK) {
      Serial.println("[server] Failed to start API server on port 80");
      return false;
    }

    register_route(s_api_httpd, "/",        HTTP_GET, root_handler);
    register_route(s_api_httpd, "/capture", HTTP_GET, capture_handler);
    register_route(s_api_httpd, "/photos",  HTTP_GET, photos_handler);
    register_route(s_api_httpd, "/photo",   HTTP_GET, photo_handler);
    register_route(s_api_httpd, "/delete",  HTTP_GET, delete_handler);
    // /stream deliberately NOT registered here — it lives only on
    // s_stream_httpd below, on its own worker task.

    Serial.println("[server] API server started on port 80");
  }
  // ── Stream server on port 81 ────────────────────────────────────────────
  // Its own worker task so the never-returning stream loop can't block
  // the API server from handling capture/gallery/download requests.
  {
    httpd_config_t cfg  = HTTPD_DEFAULT_CONFIG();
    cfg.server_port     = 81;
    cfg.ctrl_port       = 32769;
    // Only expecting one viewer at a time for this project, so 1 is
    // plenty — and keeping it minimal leaves the most possible headroom
    // for the API server above.
    cfg.max_open_sockets = 1;

    delay(500);
    if (httpd_start(&s_stream_httpd, &cfg) != ESP_OK) {
      Serial.println("[server] Failed to start stream server on port 81");
      return false;
    }

    register_route(s_stream_httpd, "/stream", HTTP_GET, stream_handler);

    Serial.println("[server] Stream server started on port 81");
  }
  return true;
}