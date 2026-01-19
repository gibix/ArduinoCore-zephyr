#ifndef ARDUINO_ZEPHYR_PDM_IMPL_H
#define ARDUINO_ZEPHYR_PDM_IMPL_H

#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/kernel.h>

#include "PDMDoubleBuffer.h"
/* size in bit of an audio sample */
/* NANO 33 BLE will work only if this value is 16 */
#define SAMPLE_BIT_WIDTH 16
#define SLAB_BLOCK_SIZE  DEFAULT_PDM_BUFFER_SIZE

namespace arduino {

int pdm_read(void **buffer, size_t *size);
int pdm_configure(int channels, int sampleRate);
int pdm_start();
int pdm_stop();
void pdm_gain(int gain);

} // namespace arduino

#endif
