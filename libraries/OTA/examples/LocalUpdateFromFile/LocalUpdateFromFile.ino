/*
 * Local OTA update from a file on the board's filesystem.
 *
 * Stages an .ota update file that is already present on /ota: and reboots into
 * it. No network connection is involved.
 *
 * To produce the .ota file, compile the sketch you want to install and take
 * the "<sketch>.elf-zsk.bin.ota" artifact from the build directory. Copy it
 * onto the board's /ota: partition as UPDATE.OTA by any means you have
 * available (for instance the LocalUpdateFromSerial example).
 *
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <OTA.h>

const char *UPDATE_FILE = "/ota:/UPDATE.OTA";

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  Serial.println("Local OTA update from file");

  if (!OTA.isOtaCapable()) {
    Serial.println("No OTA storage on this board.");
    return;
  }

  if (OTA.begin() != OTAClass::Error::None) {
    Serial.print("begin() failed: ");
    Serial.println(OTA.errorString());
    return;
  }

  Serial.print("Staging ");
  Serial.println(UPDATE_FILE);

  if (OTA.readFrom(UPDATE_FILE) < 0) {
    Serial.print("Staging failed: ");
    Serial.println(OTA.errorString());
    return;
  }

  Serial.print("Staged ");
  Serial.print(OTA.length());
  Serial.println(" bytes");

  if (OTA.update() != OTAClass::Error::None) {
    Serial.print("Update failed: ");
    Serial.println(OTA.errorString());
    return;
  }

  Serial.println("Update ready, rebooting...");
  Serial.flush();
  delay(100);

  OTA.reset();
}

void loop() {
  delay(1000);
}
