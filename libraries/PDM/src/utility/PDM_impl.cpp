/* Copyright (C) Arduino SRL (Daniele Aimo)
   SPDX-License-Identifier: MPL-2.0 */

#include "PDM_impl.h"

#include <Arduino.h>
#include <cmath>
#include <cstdint>

extern struct k_mem_slab pdm_slab;

namespace arduino {

/* ######################## ARDUINO NANO 33 BLE ############################ */

#if defined(CONFIG_BOARD_ARDUINO_NANO_33_BLE)
#include <hal/nrf_pdm.h>

static struct pcm_stream_cfg stream;
static struct dmic_cfg cfg;
/* the PDM mic zephyr device */
static const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

/* _____________________________________________________________________read */
int pdm_read(void **buffer, size_t *size) {
	return dmic_read(dmic_dev, 0, buffer, size, SYS_FOREVER_MS);
}

int pdm_configure(int channels, int sampleRate) {
	/* +++++++++ checks and verifications +++++++ */

	/* --- verify digital microphone is ready --- */
	if (!device_is_ready(dmic_dev)) {
		return -ENODEV;
	}
	/* --- check on channels --- */
	if (channels < 1 || channels > 2) {
		return -ENOTSUP; /* TODO: find the correct value */
	}
	/* --- check on sampleRate --- */
	if (!(sampleRate == 16000 || sampleRate == 41667)) {
		return -ENOTSUP; /* sample rate not supported */
	}
	/* +++++++ Set up PDM configuration ++++++++++ */
	stream.pcm_width = SAMPLE_BIT_WIDTH;
	stream.mem_slab = &pdm_slab;

	cfg.io.min_pdm_clk_freq = 1000000;
	cfg.io.max_pdm_clk_freq = 3500000;
	cfg.io.min_pdm_clk_dc = 40;
	cfg.io.max_pdm_clk_dc = 60;

	cfg.streams = &stream;
	cfg.channel.req_num_streams = 1;

	if (channels == 1) {
		cfg.channel.req_num_chan = 1;
		cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
		cfg.streams[0].pcm_rate = sampleRate;
		cfg.streams[0].block_size = SLAB_BLOCK_SIZE;
	} else {
		/* 2 channels */
		/* [TODO]: Configuration not verified on real hw */
		cfg.channel.req_num_chan = 2;
		cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) |
									  dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT);
		cfg.streams[0].pcm_rate = sampleRate;
		cfg.streams[0].block_size = SLAB_BLOCK_SIZE;
	}

	/* --- Send mic configuration to driver --- */
	return dmic_configure(dmic_dev, &cfg);
}

/* ___________________________________________________________________start() */
int pdm_start() {
	return dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
}

/* ____________________________________________________________________stop() */
int pdm_stop() {
	return dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);
}

/* _____________________________________________________________________gain()*/
void pdm_gain(int gain) {
	/* at the present the zephyr dmic_nrfx_pdm.c does not support the set
	 * of the gain (gain_l and gain_r are defined in the nrf HAL but not
	 * used by the driver which use a default value) */
	NRF_PDM->GAINR = gain;
	NRF_PDM->GAINL = gain;
}

/* ######################## ARDUINO GIGA R1 ################################ */
#elif defined(ARDUINO_GIGA)

/* Audio Configuration */
#define CLOCK_RATE (2000000U) /* 2.0 MHz */

#define BLOCK_NUM          2
#define DRIVER_BUFFER_SIZE (BLOCK_SIZE_BYTES * BLOCK_NUM)
/* The driver will use this buffer to store samples from the filter
 * data are 32 aligned to allow cache invalidation */
static int32_t __aligned(32) driver_data_buffer[BLOCK_SIZE_SAMPLES * BLOCK_NUM];

static int32_t cpy_buffer[BLOCK_SIZE_SAMPLES];

const struct device *pdm_dev = DEVICE_DT_GET(DT_NODELABEL(dfsdm1_flt0));

/* _____________________________________________________________________read */
int pdm_read(void **buffer, size_t *size) {
	void *slab_buffer;
	size_t bytes_read;
	int ret = k_mem_slab_alloc(&pdm_slab, &slab_buffer, K_NO_WAIT);
	if (ret) {
		return ret;
	}
	ret = dfsdm_read(pdm_dev, 0, (void *)cpy_buffer, BLOCK_SIZE_SAMPLES * sizeof(int32_t),
					 &bytes_read, K_FOREVER);

	uint16_t *ptr = (uint16_t *)slab_buffer;
	/* It is necessary to "squeeze" int32_t provided by dfsdm driver into int16_t
	 * accepted by PDM */
	uint32_t slab_16_dim = pdm_slab.info.block_size / sizeof(int32_t);
	uint32_t max_cp = (BLOCK_SIZE_SAMPLES < slab_16_dim) ? BLOCK_SIZE_SAMPLES : slab_16_dim;

	for (uint32_t i = 0; i < max_cp; i++) {
		*(ptr + i) = (uint16_t)(*(cpy_buffer + i));
	}
	*buffer = slab_buffer;
	*size = pdm_slab.info.block_size;
	return ret;
}

int pdm_configure(int channels, int sampleRate) {
	/* +++++++++ checks and verifications +++++++ */
	/* --- verify digital microphone is ready --- */
	if (!device_is_ready(pdm_dev)) {
		return -ENODEV;
	}
	/* --- check on channels --- */
	if (channels != 1) {
		return -ENOTSUP;
	}

	/* ---- CHANNEL CONFIGURATION ---- */
	struct dfsdm_channel_config ch_cfg = {
		.channel_id = 0,
		.source = DFSDM_SOURCE_PIN,
		.type = DFSDM_TYPE_SPI_RISING,
		.clock_mode = DFSDM_CLOCK_INTERNAL,
		.clock_source = DFSDM_INT_CLOCK_SOURCE_0,
		.output_clock_rate = CLOCK_RATE,
		.offset = 0,
		.right_bit_shift = 0,
	};

	struct dfsdm_channel_config {
		uint8_t channel_id;
		enum dfsdm_channel_source source;
		uint8_t pin_group;
		enum dfsdm_channel_type type;
		enum dfsdm_channel_clock_mode clock_mode;
		enum dfsdm_internal_ck_source clock_source;
		uint32_t output_clock_rate;
		uint32_t offset;
		uint8_t right_bit_shift;
	};

	int err = dfsdm_configure_channel(pdm_dev, &ch_cfg);

	if (err) {
		return err;
	}

	/* ----- STREAM CONFIGURATION (fiter settings) */
	struct dfsdm_stream_config stream_cfg = {
		.filter =
			{
				.filter_id = 0,
				.channel_id = 0,
				.sinc_type = DFSDM_SINC_FAST,
				.oversampling = (CLOCK_RATE / sampleRate),
				.integrator = 0,
			},

		.data_buffer = driver_data_buffer,
		.data_buffer_size = DRIVER_BUFFER_SIZE,
		.invalidate_cache = true,
		.discard_first_n_block = 1,
		.callback = NULL, // do no use callback
		.user_data = NULL,
	};

	err = dfsdm_configure_stream(pdm_dev, &stream_cfg);

	if (err) {
		return err;
	}
	return 0;
}

/* ___________________________________________________________________start() */
int pdm_start() {
	return dfsdm_trigger(pdm_dev, 0, DFSDM_TRIGGER_START);
}

/* ____________________________________________________________________stop() */
int pdm_stop() {
	return dfsdm_trigger(pdm_dev, 0, DFSDM_TRIGGER_STOP);
}

/* _____________________________________________________________________gain()*/
void pdm_gain(int gain) {
	(void)gain;
	/* In case of dfsdm the gain cannot be implemented:
	 * - the sample is making samples which are 24 bits wide
	 * - those sample are "squeezed" into a 16 bits by the read function
	 *
	 * to make the gain in the mbed version of this library samples were left
	 * shifted, but this makes no sense because of what said above. */
}
#endif
} // namespace arduino
