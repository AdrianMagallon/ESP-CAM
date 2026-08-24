#pragma once

#include <stdbool.h>

/**
 * Initialise the OV3660 camera.
 * Must be called before any call to esp_camera_fb_get().
 * Returns true on success, false on hardware or config error.
 */
bool initCamera();