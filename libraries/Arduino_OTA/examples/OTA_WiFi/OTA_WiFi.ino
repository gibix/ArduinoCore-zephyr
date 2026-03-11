/*
 * OTA WiFi Update Example
 *
 * Downloads a firmware update (.ota file) over HTTP and applies it.
 * The .ota file includes an OTA header with LZSS compression and CRC-32.
 *
 * Supported boards: Portenta H7, Giga R1, Opta, Portenta C33
 *
 * To use:
 * 1. Build your sketch normally — the .ota file is created automatically
 * 2. Host the .ota file on an HTTP server:
 *    python3 -m http.server 8080
 * 3. Upload this sketch via USB
 * 4. The board downloads the .ota, verifies CRC, and prepares the update
 * 5. On reboot, the bootloader/SFU applies the update from QSPI flash
 *
 * NOTE: The QSPI flash must have a valid MBR partition table
 * (present on boards previously used with the stock Arduino firmware).
 * For Portenta C33, the SFU binary must be flashed to 0x10000 (one-time setup).
 */

#include <Arduino_OTA.h>
#include <WiFi.h>
#include "arduino_secrets.h"

// Board-specific OTA magic numbers
#if defined(ARDUINO_PORTENTA_H7_M7)
#define OTA_BOARD_MAGIC 0x2341025B
#elif defined(ARDUINO_GIGA)
#define OTA_BOARD_MAGIC 0x23410266
#elif defined(ARDUINO_OPTA)
#define OTA_BOARD_MAGIC 0x23410064
#elif defined(ARDUINO_PORTENTA_C33)
#define OTA_BOARD_MAGIC 0x23410068
#else
#error "OTA not supported on this board"
#endif

void setup() {
  Serial.begin(115200);
  while (!Serial);

  // check for the WiFi module:
  if (WiFi.status() == WL_NO_SHIELD) {
    Serial.println("Communication with WiFi module failed!");
    // don't continue
    while (true)
      ;
  }

  WiFi.begin(SECRET_SSID, SECRET_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println(WiFi.status());
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");

  ArduinoOTA.setURL("http://192.168.1.102:8000/Blink.ino.ota");
  ArduinoOTA.setMagic(OTA_BOARD_MAGIC);

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
