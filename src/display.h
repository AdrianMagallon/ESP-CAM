#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Init the TFT display and touch controller. Call once in setup().
 */
bool initDisplay();

uint16_t displayWidth();
uint16_t displayHeight();

/**
 * Fills the screen black. Needed before switching between UI views
 * (camera / gallery) so old content doesn't linger under the new view.
 */
void displayClear();

/**
 * Restricts subsequent drawing (including displayDrawJpeg) to the given
 * rectangle — used to keep the live camera feed from painting over a
 * toolbar drawn below it. Call displayResetViewport() when done.
 */
void displaySetViewport(int16_t x, int16_t y, int16_t w, int16_t h);
void displayResetViewport();

/**
 * Draws a simple labeled button rectangle at the given position.
 */
void displayDrawButton(int16_t x, int16_t y, int16_t w, int16_t h, const char* label);

/**
 * Draws a small status banner near the top-center of the screen — for
 * one-off feedback like "Photo saved", not a tap target. Deliberately
 * styled differently from displayDrawButton so it doesn't look pressable.
 * Caller is responsible for redrawing over it once it should disappear.
 */
void displayDrawToast(const char* msg);

/**
 * Checks for a new tap. Returns true once per tap, with coordinates
 * already mapped to screen pixels and debounced — safe to call every
 * loop() with no extra timing logic on your end.
 */
bool displayGetTouch(int16_t* outX, int16_t* outY);

/**
 * Decodes a JPEG buffer straight to the screen. `brightness` is 0-255
 * (255 = full brightness) — since our backlight has no PWM pin wired,
 * this fakes dimming by scaling the decoded pixel colors instead.
 */
void displayDrawJpeg(const uint8_t* jpegBuf, size_t len, uint8_t brightness);