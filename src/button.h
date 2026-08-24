#pragma once
#include <stdbool.h>
#include <stdint.h>

/**
 * Set up the button pin and attach the tap-counting interrupt. Call once
 * in setup().
 */
void initButton(uint8_t pin);

/**
 * Returns true once you've tapped the button 5 times in a row (each tap
 * within ~2 seconds of the last one). Debounced internally — safe to call
 * every loop() with no extra timing logic on your end.
 */
bool buttonActivated();

/**
 * Detaches the tap-counting interrupt from the button pin. MUST be called
 * before configuring the pin as a deep-sleep wake source — otherwise the
 * interrupt keeps firing on sensor noise/bounce during the sleep-entry
 * window, corrupting the tap count and/or contributing to instant re-wake.
 */
void disableButtonInterrupt();