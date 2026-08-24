#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Start two HTTP servers:
 *   Port 80 — UI page + REST API (capture, list, serve, delete photos)
 *   Port 81 — MJPEG stream only, on its own worker task, so the
 *             never-returning stream loop can't block API requests
 *             like /capture, /photos, /photo, /delete.
 *
 * Requires initCamera() and initStorage() to have succeeded first.
 */
bool startServers();

/**
 * Copy the latest camera frame into the caller's buffer, growing it if
 * needed (caller owns *buf and must free() it eventually). This is the
 * same shared frame the WiFi stream and /capture read from — does NOT
 * talk to the camera driver directly. Returns false if no frame is
 * ready yet or the mutex was busy.
 */
bool getLatestFrameForDisplay(uint8_t** buf, size_t* cap, size_t* outLen);