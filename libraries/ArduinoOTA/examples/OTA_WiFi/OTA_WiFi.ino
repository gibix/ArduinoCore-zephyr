/*
 * OTA WiFi Update Example
 *
 * Downloads a firmware update (.ota file) over HTTP and applies it.
 *
 * To use:
 * 1. Build your sketch normally — the .ota file is created automatically
 * 2. Host the .ota file on an HTTP server:
 *    python3 -m http.server 8080
 * 3. Upload this sketch via USB
 * 4. The board downloads the .ota, writes it to QSPI, and reboots
 * 5. The bootloader applies the update from QSPI flash
 *
 * NOTE: The QSPI flash must have a valid MBR partition table
 * (present on boards previously used with the stock Arduino firmware).
 */

#include <ArduinoOTA.h>
#include <WiFi.h>
#include "arduino_secrets.h"

void setup() {
  Serial.begin(115200);
  while (!Serial);

  WiFi.begin(SECRET_SSID, SECRET_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");

  ArduinoOTA.setURL("http://192.168.1.100:8080/sketch.ota");
  ArduinoOTA.begin();

  Serial.println("Downloading firmware...");
  int err = ArduinoOTA.download();
  if (err == 0) {
    Serial.println("Download OK, applying update...");
    delay(500);
    ArduinoOTA.update();  // reboots, does not return
  } else {
    Serial.print("Error: ");
    Serial.println(ArduinoOTA.errorString());
  }
}

void loop() {
}
