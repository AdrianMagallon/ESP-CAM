#include "camera.h"
#include "esp_camera.h"
#include "Arduino.h"

// ── Pin map for Freenove-style ESP32-S3 board with OV3660 ────────
// If you swap boards, only this block needs to change.
#define CAM_PIN_PWDN    -1   // Power-down not wired on this board
#define CAM_PIN_RESET   -1   // Hardware reset not wired; software reset used
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4   // SCCB data (I2C SDA equivalent)
#define CAM_PIN_SIOC     5   // SCCB clock (I2C SCL equivalent)
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2       8
#define CAM_PIN_D1       9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13

// ── Tuning constants — adjust here, not buried in code ───────────
#define CAM_XCLK_FREQ_HZ  20000000   // 20 MHz — safe default for OV3660
#define CAM_JPEG_QUALITY  12         // more compression, smaller frames
#define CAM_FRAME_SIZE    FRAMESIZE_VGA   // 480x320 — meaningful drop from VGA's 640x480
#define CAM_FB_COUNT      2          // Double-buffer: smoother stream, needs PSRAM

bool initCamera() {
  camera_config_t config = {};

  // LEDC peripheral drives the XCLK signal to the camera
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  // Data lines (8-bit parallel interface)
  config.pin_d0    = CAM_PIN_D0;
  config.pin_d1    = CAM_PIN_D1;
  config.pin_d2    = CAM_PIN_D2;
  config.pin_d3    = CAM_PIN_D3;
  config.pin_d4    = CAM_PIN_D4;
  config.pin_d5    = CAM_PIN_D5;
  config.pin_d6    = CAM_PIN_D6;
  config.pin_d7    = CAM_PIN_D7;

  // Control lines
  config.pin_xclk      = CAM_PIN_XCLK;
  config.pin_pclk      = CAM_PIN_PCLK;
  config.pin_vsync     = CAM_PIN_VSYNC;
  config.pin_href      = CAM_PIN_HREF;
  config.pin_sccb_sda  = CAM_PIN_SIOD;
  config.pin_sccb_scl  = CAM_PIN_SIOC;
  config.pin_pwdn      = CAM_PIN_PWDN;
  config.pin_reset     = CAM_PIN_RESET;

  config.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
  config.pixel_format = PIXFORMAT_JPEG; // JPEG output required for MJPEG stream

  if (psramFound()) {
    // PSRAM available (expected on N16R8) — use double buffering for smooth stream
    config.frame_size   = CAM_FRAME_SIZE;
    config.jpeg_quality = CAM_JPEG_QUALITY;
    config.fb_count     = CAM_FB_COUNT;
    config.grab_mode    = CAMERA_GRAB_LATEST; // always give us the newest frame
    Serial.println("[camera] PSRAM found — using double buffer");
  } else {
    // Fallback: smaller frame, single buffer to fit in internal RAM
    Serial.println("[camera] No PSRAM — falling back to QVGA single buffer");
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 20;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[camera] Init failed (0x%x) — check wiring and pin map\n", err);
    return false;
  }

  // ── Sensor tuning ─────────────────────────────────────────────
  // These can also be changed at runtime via the sensor_t pointer.
  sensor_t* s = esp_camera_sensor_get();
  if (s != nullptr) {
    s->set_brightness(s, 0);       // -2 to 2
    s->set_contrast(s, 0);         // -2 to 2
    s->set_saturation(s, 0);       // -2 to 2
    s->set_whitebal(s, 1);         // auto white balance on
    s->set_awb_gain(s, 1);         // AWB gain on
    s->set_exposure_ctrl(s, 1);    // auto exposure on
    s->set_aec2(s, 1);             // AEC DSP on
    s->set_gainceiling(s, (gainceiling_t)2);
    s->set_vflip(s, 1);            // set to 1 if image is upside-down
    s->set_hmirror(s, 1);          // set to 1 if image is mirrored
  }

  Serial.println("[camera] Init OK");
  return true;
}