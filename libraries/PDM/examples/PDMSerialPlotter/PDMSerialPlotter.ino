/*
  Nano 33 BLE Microphone to Serial Plotter
*/
#include <PDM.h>

#ifndef CONFIG_BOARD_ARDUINO_NANO_33_BLE
#error "Only Nano 33 BLE board is currently supported by this library"
#endif
// default number of output channels
// Nano 33 BLE only supports 1 channel
static const char channels = 1;
// default PCM output frequency
static const int frequency = 16000;
// Buffer to read samples into
// For better performance set the user buffer dimension to the dimension
// of the buffer used by PDM library this way
short sampleBuffer[DEFAULT_PDM_BUFFER_SIZE];
// Number of bytes read
volatile int samplesRead;

void setup() {
	Serial.begin(115200);
	while (!Serial)
		;

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
	// Wait for samples to be read
	if (samplesRead) {

		// Print samples to the serial monitor or plotter
		for (int i = 0; i < samplesRead; i++) {
			if (channels == 2) {
				Serial.print("L:");
				Serial.print(sampleBuffer[i]);
				Serial.print(" R:");
				i++;
			}
			Serial.println(sampleBuffer[i]);
		}

		// Clear the read count
		samplesRead = 0;
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
