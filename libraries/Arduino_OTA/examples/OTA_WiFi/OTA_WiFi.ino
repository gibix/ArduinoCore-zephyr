/*
 * OTA WiFi Update Example
 *
 * Downloads a firmware update (.ota file) over HTTP and applies it.
 * The .ota file includes an OTA header with LZSS compression and CRC-32.
 *
 * To use:
 * 1. Build your sketch normally — the .ota file is created automatically
 * 2. Host the .ota file on an HTTP server:
 *    python3 -m http.server 8080
 * 3. Upload this sketch via USB
 * 4. The board downloads the .ota, verifies CRC, decompresses LZSS,
 *    writes UPDATE.BIN to QSPI, and reboots
 * 5. The bootloader applies the update from QSPI flash
 *
 * NOTE: The QSPI flash must have a valid MBR partition table
 * (present on boards previously used with the stock Arduino firmware).
 */

#include <Arduino_OTA.h>
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
  ArduinoOTA.setMagic(0x2341025B);  // Portenta H7

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
