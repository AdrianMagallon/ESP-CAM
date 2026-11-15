#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Longest filename (including the terminating '\0') the rest of the code
// has to make room for. Our own names are "photo_<number>.jpg", which is
// 20 characters at the very most.
#define PHOTO_NAME_MAX 32

/**
 * Mount the SD card over SD_MMC. Call once at boot before any other storage functions.
 * Returns false if there's no usable card — the rest of the firmware can
 * keep running without one (streaming still works, saving doesn't).
 */
bool initStorage();

/**
 * True if initStorage() mounted a card. Every function below fails
 * cleanly (returns false / 0) when this is false.
 */
bool storageAvailable();

/**
 * True if `name` looks like one of our photo files: starts with
 * "photo_", fits in PHOTO_NAME_MAX, and has no path separators or ".."
 * in it. Anything that comes in over HTTP has to pass this before it
 * gets anywhere near the filesystem.
 */
bool isValidPhotoName(const char* name);

/**
 * Save a JPEG frame to the SD card. nameOut receives the generated
 * filename (e.g. "photo_42.jpg"). Names count upward and carry on from
 * the highest number already on the card, so they never repeat across
 * reboots. Returns true only if every byte made it onto the card.
 */
bool savePhoto(const uint8_t* data, size_t len, char* nameOut, size_t nameOutLen);

/**
 * Calls cb(name, ctx) once for every saved photo, in directory order.
 * Return false from the callback to stop early. Returns the number of
 * photos visited.
 */
typedef bool (*PhotoVisitor)(const char* name, void* ctx);
int forEachPhoto(PhotoVisitor cb, void* ctx);

/**
 * Delete a photo by filename (e.g. "photo_42.jpg"). Returns true on success.
 */
bool deletePhoto(const char* name);

/**
 * Construct the full filesystem path for a given photo filename.
 * Centralises path logic so the rest of the code doesn't hardcode it.
 */
void getPhotoPath(const char* name, char* pathOut, size_t pathLen);
