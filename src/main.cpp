bool roleAssigned = false;
bool clientConnected = false;
bool doConnect = false;
bool doScan = false;
bool doRoleNegotiation = false;

#include <Arduino.h>
#include "BLESync.h"


void setup() {
  Serial.begin(115200);
  delay(1000);
  BLESync_setup();
}

void loop() {
  BLESync_loop();
}