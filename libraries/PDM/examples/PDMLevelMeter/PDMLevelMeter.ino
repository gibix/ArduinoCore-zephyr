/* Copyright (C) Arduino SRL
 * SPDX-License-Identifier: MPL-2.0 */

/*
  PDM microphone level meter — quick smoke test.

  Prints peak absolute sample value per buffer block as a text bar to
  the serial console. Tap or speak near the mic and the bar should
  jump. Works on Nano 33 BLE, Nano RP2040 Connect, and GIGA.

  No host-side tools needed — open the serial monitor at 115200.
*/

#include <PDM.h>

#if !defined(CONFIG_BOARD_ARDUINO_NANO_33_BLE) && !defined(ARDUINO_GIGA) && \
	!defined(CONFIG_BOARD_ARDUINO_NANO_CONNECT)
#error "Only Nano 33 BLE, Arduino GIGA or Nano RP2040 Connect boards are currently supported"
#endif

static const int channels = 1;
static const int frequency = 16000;

short sampleBuffer[DEFAULT_PDM_BUFFER_SIZE];

void setup() {
	Serial.begin(115200);
	while (!Serial)
		;

	if (!PDM.begin(channels, frequency)) {
		Serial.println("Failed to start PDM!");
		while (1)
			;
	}
}

void loop() {
	int bytesAvailable = PDM.available();
	if (bytesAvailable <= 0) {
		return;
	}

	PDM.read(sampleBuffer, bytesAvailable);
	int samples = bytesAvailable / 2;

	int peak = 0;
	for (int i = 0; i < samples; i++) {
		int v = sampleBuffer[i];
		if (v < 0) {
			v = -v;
		}
		if (v > peak) {
			peak = v;
		}
	}

	// 50-cell bar, full scale = 32767
	int bar = peak / 655;
	if (bar > 50) {
		bar = 50;
	}

	Serial.print('[');
	for (int i = 0; i < 50; i++) {
		Serial.print(i < bar ? '#' : ' ');
	}
	Serial.print("] ");
	Serial.println(peak);
}
