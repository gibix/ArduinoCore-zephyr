#ifndef ARDUINO_ZEPHYR_PDM_H
#define ARDUINO_ZEPHYR_PDM_H

#include <Arduino.h>
#include <cstdint>
#include "./utility/PDM_impl.h"

namespace arduino {

class PDMClass {
public:
	PDMClass();
	virtual ~PDMClass();
	/* support 1 or 2 channels, sampleRate can be 16000 or 41667 */
	int begin(int channels, int sampleRate);
	void end();
	virtual int available();
	virtual int read(void *buffer, size_t size);
	void onReceive(void (*)(void));
	void setGain(int gain);
	size_t getBufferSize();

private:
	bool pdm_init;
	bool active;
	PDMDoubleBuffer db;
};

} // namespace arduino

typedef arduino::PDMClass PDMClass;

extern PDMClass PDM;

#endif // ARDUINO_ZEPHYR_PDM_H
