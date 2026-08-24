#pragma once
#include <stdbool.h>

/**
 * Set up button layout. Call once in setup(), after initDisplay().
 */
void initLocalUI();

/**
 * Draws the latest camera frame + button overlay, and handles taps.
 * Call every loop().
 */
void updateLocalUI();