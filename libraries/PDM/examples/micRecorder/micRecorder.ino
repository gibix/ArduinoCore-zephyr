/*
  Nano 33 BLE Microphone to Serial Streamer

  How to use this example
  -----------------------

  Download this sketch into the Nano33 BLE
  This will start immediately mic acquisition
  Be sure that serial monitor or any other program is not accessing the Nano33
  serial port
  Start the python script present in the same folder (getWawe.py)
  This python script will get the data streamed by this sketch in the
  serial port and pack them into a wav file you can listen with any player on
  your PC
  NOTE:
  the python script uses /dev/ttyACM0 as default serial device, change it
  accordingly if the nano33 serial does not correspond to this device
  NOTE: the python script only records about 5 seconds of sounds
*/
#include <PDM.h>

#ifndef CONFIG_BOARD_ARDUINO_NANO_33_BLE
#error "Only Nano 33 BLE board is currently supported by this library"
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

		// Write the raw bytes to the Serial port
		// We send byte-by-byte to keep it fast and binary
		Serial.write((uint8_t *)sampleBuffer, samplesRead * 2);
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
