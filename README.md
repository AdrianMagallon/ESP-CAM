# ESP-CAM

A standalone WiFi camera built on a Freenove ESP32-S3-WROOM board (N16R8 — 16MB flash, 8MB OPI PSRAM) with an OV3660 camera module. It doesn't need a home router: the board broadcasts its own access point, and any phone or laptop that joins it gets a live MJPEG stream and a simple photo gallery straight from the browser. A small touchscreen wired to the same board mirrors that same camera/gallery experience locally, so it also works as a self-contained handheld camera.

## What it does

- **Boots as its own WiFi access point** (`ESP32-S3-Cam`, open only to devices that know the password) instead of joining an existing network, so it works anywhere with no setup.
- **Streams live video over HTTP** as MJPEG on port 81, and serves a single-page web app on port 80 with a camera view and a photo gallery view.
- **Captures and stores photos on a microSD card**, mounted over 1-bit SD_MMC, in a flat `/photos` directory. The web UI and the on-device UI can both browse, view, and delete saved photos.
- **Drives a local touchscreen** (TFT_eSPI + TJpg_Decoder for JPEG-to-screen rendering) with the same two views — live camera and gallery — including on-screen buttons for capture, gallery navigation, and delete. Touch calibration runs once and is cached in flash (NVS) so it doesn't need to be redone on every boot.
- **Watches its own WiFi radio.** A low-priority background task checks every 10 seconds that the access point is still up and automatically brings it back if it ever drops, since the streaming server otherwise has no way to notice a dead radio on its own.
- **Reacts to a physical touch button** wired to pin 14 — five taps within a 2-second window register as a deliberate gesture (debounced in an interrupt handler) rather than a router that's easy to trigger by accident.

## Hardware

- Freenove ESP32-S3-WROOM (N16R8)
- OV3660 camera module on the 8-bit parallel/SCCB interface
- microSD card over 1-bit SD_MMC (shares some data lines with the camera, so pin selection is tight)
- SPI TFT display with resistive/capacitive touch (TFT_eSPI-compatible), pinned out in `include/user_setup.h`
- Capacitive touch button (TTP223-style) on GPIO14

The `Hardware/` folder has the KiCad schematic (`Esp-Camera.kicad_sch` / `.kicad_pro`) and an exported `Schematic.pdf` for the board wiring.

## How it's built

Firmware is written in C++ against the Arduino framework, built and flashed with [PlatformIO](https://platformio.org/) (see `platformio.ini` — target is `freenove_esp32_s3_wroom`, 16MB flash, OPI PSRAM, a custom 16MB partition table for the app + SD/FS split). The two external library dependencies are `TFT_eSPI` (display driver) and `TJpg_Decoder` (JPEG decoding for the local screen).

The code is split into one module per responsibility, each with a `.h`/`.cpp` pair under `src/`:

| Module | Responsibility |
|---|---|
| `camera` | Configures and initializes the OV3660 over the ESP32 camera driver, with a PSRAM-aware fallback (drops to QVGA/single-buffer if PSRAM isn't found) |
| `wifi_ap` | Brings up the WiFi access point, disables radio power-save to cut stream stutter, and runs the watchdog task that keeps the AP alive |
| `storage` | Mounts the SD card and handles saving, listing, and deleting photos |
| `stream_server` | Runs two HTTP servers (API on port 80, MJPEG stream on port 81) and serves the web UI |
| `button` | Debounced interrupt handler for the physical touch button, counting taps in a rolling window |
| `display` | Low-level TFT driving: JPEG-to-screen rendering, touch calibration (cached in NVS), brightness scaling, and drawing primitives (buttons, toasts) |
| `local_ui` | The on-device camera/gallery screens, built on top of `display`, `stream_server`, `storage`, and `button` |

`main.cpp` brings all of these up in order at boot — camera, storage, WiFi AP (plus the watchdog), HTTP servers, display, then the button and local UI — halting with a serial error message if any stage fails, since a partially-initialized board is more confusing to debug than one that stops cleanly.

## Getting started

1. Install [PlatformIO](https://platformio.org/) (CLI or the VS Code extension).
2. Wire up the hardware per `Hardware/Schematic.pdf`, or adapt the pin definitions at the top of `src/camera.cpp`, `src/storage.cpp`, and `include/user_setup.h` if you're using a different board layout.
3. Open `src/main.cpp` and change `WIFI_SSID` / `WIFI_PASS` before flashing — the defaults are placeholders and shouldn't be shipped as-is.
4. Insert a microSD card, then build and upload:
   ```
   pio run --target upload
   ```
5. Connect to the `ESP32-S3-Cam` access point from a phone or laptop and open `http://<device-ip>/` (the IP is also printed over serial at 115200 baud on boot).

## License

MIT — see [LICENSE](LICENSE).
