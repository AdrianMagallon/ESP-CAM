#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Mount the SD card over SD_MMC. Call once at boot before any other storage functions.
 */
bool initStorage();

/**
 * Save a JPEG frame to flash. nameOut receives the generated filename (e.g. "photo_12345.jpg").
 * Returns true on success.
 */
bool savePhoto(const uint8_t* data, size_t len, char* nameOut, size_t nameOutLen);

/**
 * Write a JSON array of photo filenames into buf, e.g. ["photo_1.jpg","photo_2.jpg"].
 * Returns true on success.
 */
bool listPhotos(char* buf, size_t bufLen);

/**
 * Delete a photo by filename (e.g. "photo_12345.jpg"). Returns true on success.
 */
bool deletePhoto(const char* name);

/**
 * Construct the full filesystem path for a given photo filename.
 * Centralises path logic so the rest of the code doesn't hardcode it.
 */
void getPhotoPath(const char* name, char* pathOut, size_t pathLen);