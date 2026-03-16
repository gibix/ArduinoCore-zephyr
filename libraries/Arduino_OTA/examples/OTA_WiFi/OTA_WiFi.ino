/*
 * OTA WiFi Update Example
 *
 * Downloads a firmware update (.ota file) over HTTP and applies it.
 * The .ota file includes an OTA header with LZSS compression and CRC-32.
 *
 * Works on any board with WiFi + QSPI filesystem (OTA storage).
 *
 * To use:
 * 1. Build your sketch normally — the .ota file is created automatically
 * 2. Host the .ota file on an HTTP server:
 *    python3 -m http.server 8080
 * 3. Upload this sketch via USB
 * 4. The board downloads the .ota, verifies CRC, decompresses to QSPI
 * 5. On reboot, the loader validates the new sketch and flashes it
 */

#include <Arduino_OTA_Loader.h>
#include <WiFi.h>
#include "arduino_secrets.h"

ArduinoOTALoaderClass ArduinoOTA;

void setup() {
  Serial.begin(115200);
  while (!Serial);

  if (WiFi.status() == WL_NO_SHIELD) {
    Serial.println("Communication with WiFi module failed!");
    while (true);
  }

  WiFi.begin(SECRET_SSID, SECRET_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println(WiFi.status());
    delay(3000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");

  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  ArduinoOTA.setURL("http://192.168.1.102:8000/Blink.ino.ota");

  if (ArduinoOTA.begin() != ArduinoOTAClass::Error::None) {
    Serial.print("Begin failed: ");
    Serial.println(ArduinoOTA.errorString());
    return;
  }

  Serial.println("Downloading...");
  int bytes = ArduinoOTA.download();
  if (bytes < 0) {
    Serial.print("Download failed: ");
    Serial.println(ArduinoOTA.errorString());
    return;
  }
  Serial.print("Downloaded ");
  Serial.print(bytes);
  Serial.println(" bytes");

  Serial.println("Verifying and decompressing...");
  int size = ArduinoOTA.decompress();
  if (size < 0) {
    Serial.print("Decompress failed: ");
    Serial.println(ArduinoOTA.errorString());
    return;
  }
  Serial.print("Decompressed size: ");
  Serial.print(size);
  Serial.println(" bytes");

  if (ArduinoOTA.update() != ArduinoOTAClass::Error::None) {
    Serial.print("Update failed: ");
    Serial.println(ArduinoOTA.errorString());
    return;
  }

  Serial.println("Rebooting...");
  delay(500);
  ArduinoOTA.reset();
}

void loop() {
}
