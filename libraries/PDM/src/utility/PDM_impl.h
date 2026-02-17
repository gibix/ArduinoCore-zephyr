/* Copyright (C) Arduino SRL (Daniele Aimo)
 *    SPDX-License-Identifier: MPL-2.0 */

#ifndef ARDUINO_ZEPHYR_PDM_IMPL_H
#define ARDUINO_ZEPHYR_PDM_IMPL_H

#if defined(CONFIG_BOARD_ARDUINO_NANO_33_BLE)
#include <zephyr/audio/dmic.h>
#elif defined(ARDUINO_GIGA)
#include <zephyr/drivers/dfsdm.h>
#endif
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/kernel.h>

#include "PDMDoubleBuffer.h"
/* size in bit of an audio sample */
/* NANO 33 BLE will work only if this value is 16 */
#define SAMPLE_BIT_WIDTH 16
#if defined(CONFIG_BOARD_ARDUINO_NANO_33_BLE)
#define SLAB_BLOCK_NUM  4
#define SLAB_ALIGN      4
#define SLAB_BLOCK_SIZE DEFAULT_PDM_BUFFER_SIZE
/* SLAB configuration */
#elif defined(ARDUINO_GIGA)
/* SLAB define are used to create a slab that is used to take the data
 * provided by the filter (32 bits per sample) to the user buffer (16 bits per
 * samples)*/

#define SLAB_BLOCK_NUM     2
#define SLAB_ALIGN         32
#define BLOCK_SIZE_SAMPLES 256 /* ~16ms at 2MHz  */
#define BLOCK_SIZE_BYTES   (BLOCK_SIZE_SAMPLES * sizeof(int32_t))
#define SLAB_BLOCK_SIZE    (BLOCK_SIZE_BYTES * sizeof(int16_t))
#endif

namespace arduino {

int pdm_read(void **buffer, size_t *size);
int pdm_configure(int channels, int sampleRate);
int pdm_start();
int pdm_stop();
void pdm_gain(int gain);

} // namespace arduino

#endif
