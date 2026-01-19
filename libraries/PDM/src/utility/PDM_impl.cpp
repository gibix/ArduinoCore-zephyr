#include "PDM_impl.h"

#include <Arduino.h>

extern struct k_mem_slab pdm_slab;

namespace arduino {

/* ######################## ARDUINO NANO 33 BLE ############################ */

#ifdef CONFIG_BOARD_ARDUINO_NANO_33_BLE

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
#endif
} // namespace arduino
