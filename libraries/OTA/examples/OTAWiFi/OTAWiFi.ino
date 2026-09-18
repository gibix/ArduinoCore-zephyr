/*
 * OTA sketch update over WiFi.
 *
 * Downloads an .ota update file over plain HTTP and asks the loader to install
 * it on the next boot.
 *
 * To produce the .ota file, compile the sketch you want to install and take the
 * "<sketch>.elf-zsk.bin.ota" artifact from the build directory, then serve it
 * from any HTTP server, for example:
 *
 *   python3 -m http.server 8000
 *
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ZephyrClient.h>
#include <WiFi.h>
#include <OTA.h>

#include "arduino_secrets.h"
///////please enter your sensitive data in the Secret tab/arduino_secrets.h
char ssid[] = SECRET_SSID;  // your network SSID (name)
char pass[] = SECRET_PASS;  // your network password

const char *server = "192.168.1.10";  // host serving the update
const uint16_t port = 8000;
const char *path = "/UPDATE.ota";

ZephyrClient client;

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  Serial.println("OTA update over WiFi");

  if (!OTA.isOtaCapable()) {
    Serial.println("No OTA storage on this board.");
    return;
  }

  Serial.print("Connecting to ");
  Serial.println(ssid);
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) {
    Serial.println("Connection failed, retrying...");
    delay(5000);
  }

  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());

  if (OTA.begin() != OTAClass::Error::None) {
    Serial.print("begin() failed: ");
    Serial.println(OTA.errorString());
    return;
  }

  Serial.print("Downloading http://");
  Serial.print(server);
  Serial.println(path);

  if (OTA.readFrom(client, server, port, path) < 0) {
    Serial.print("Download failed: ");
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
