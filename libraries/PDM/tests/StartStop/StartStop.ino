/* Copyright (C) Arduino SRL (Daniele Aimo)
 *  * SPDX-License-Identifier: MPL-2.0 */

/*
  Microphone to Serial Streamer - Start and Stop acquisition
*/
#include <PDM.h>

#if !defined(CONFIG_BOARD_ARDUINO_NANO_33_BLE) && !defined(ARDUINO_GIGA)
#error "Only Nano 33 BLE or Arduino GIGA boards are currently supported by this library"
#endif

// default number of output channels
static const char channels = 1;
// default PCM output frequency
static const int frequency = 16000;
// Buffer to read samples into
// For better performance set the user buffer dimension to the dimension
// of the buffer used by PDM library this way
short sampleBuffer[DEFAULT_PDM_BUFFER_SIZE];
// Number of bytes read
volatile int samplesRead;

bool status_on = true;

void setup() {
	Serial.begin(115200);
	while (!Serial)
		;

	while (Serial.available()) {
		Serial.read();
	}

	// Configure the data receive callback
	PDM.onReceive(onPDMdata);

	// Initialize PDM:
	// - one channel (Mono)
	// - 16 kHz sample rate (Standard for voice)
	if (!PDM.begin(channels, frequency)) {
		Serial.println("Failed to start PDM!");
		while (1)
			;
	}
}

void loop() {
	static unsigned long int time = millis();

	// Wait for samples to be read
	if (samplesRead) {

		// Write the raw bytes to the Serial port
		// We send byte-by-byte to keep it fast and binary
		Serial.write((uint8_t *)sampleBuffer, samplesRead * 2);
		// Clear the read count
		samplesRead = 0;
	}
	if (millis() - time > 5000) {
		time = millis();
		if (status_on) {
			Serial.println("PDM stopped!");
			PDM.end();
			status_on = false;
		} else {
			if (PDM.begin(channels, frequency)) {
				status_on = true;
			} else {
				Serial.println("FAILED to start PDM!");
			}
		}
	}
}

// Callback function: Handling the PDM on receive event
// The PDM library will call this callback function as soon as the internal
// buffer is ready and can be read
// It is user responsibility to read from PDM as fast as possible otherwise
// data will be lost
void onPDMdata() {
	// Query the number of bytes available
	int bytesAvailable = PDM.available();
	// Read into the sample buffer
	PDM.read(sampleBuffer, bytesAvailable);

	// 16-bit, 2 bytes per sample
	samplesRead = bytesAvailable / 2;
}
