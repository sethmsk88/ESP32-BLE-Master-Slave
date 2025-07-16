#pragma once
#include <Arduino.h>

// Call this in setup()
void BLESync_setup();

// Call this in loop()
void BLESync_loop();

// Optionally, expose resetConnectionState if needed elsewhere
void resetConnectionState();
