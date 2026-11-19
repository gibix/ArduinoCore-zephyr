/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Retained memory in a range of an EEPROM: on native_sim the EEPROM is a
 * host file, so the area survives the end of the process like retained RAM
 * survives a warm reset.
 */

#define DT_DRV_COMPAT arduino_retained_eeprom

#include <zephyr/device.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/retained_mem.h>

struct retained_eeprom_config {
	const struct device *eeprom;
	off_t offset;
	size_t size;
};

static ssize_t retained_eeprom_size(const struct device *dev)
{
	const struct retained_eeprom_config *cfg = dev->config;

	return (ssize_t)cfg->size;
}

static int retained_eeprom_read(const struct device *dev, off_t offset, uint8_t *buffer,
				size_t size)
{
	const struct retained_eeprom_config *cfg = dev->config;

	return eeprom_read(cfg->eeprom, cfg->offset + offset, buffer, size);
}

static int retained_eeprom_write(const struct device *dev, off_t offset, const uint8_t *buffer,
				 size_t size)
{
	const struct retained_eeprom_config *cfg = dev->config;

	return eeprom_write(cfg->eeprom, cfg->offset + offset, buffer, size);
}

static int retained_eeprom_clear(const struct device *dev)
{
	const struct retained_eeprom_config *cfg = dev->config;
	uint8_t zero[16] = {0};
	int rc = 0;

	for (size_t off = 0; off < cfg->size && rc == 0; off += sizeof(zero)) {
		rc = eeprom_write(cfg->eeprom, cfg->offset + off, zero,
				  MIN(sizeof(zero), cfg->size - off));
	}
	return rc;
}

static int retained_eeprom_init(const struct device *dev)
{
	const struct retained_eeprom_config *cfg = dev->config;

	return device_is_ready(cfg->eeprom) ? 0 : -ENODEV;
}

static DEVICE_API(retained_mem, retained_eeprom_api) = {
	.size = retained_eeprom_size,
	.read = retained_eeprom_read,
	.write = retained_eeprom_write,
	.clear = retained_eeprom_clear,
};

#define RETAINED_EEPROM_DEFINE(n)                                                                  \
	static const struct retained_eeprom_config retained_eeprom_config_##n = {                  \
		.eeprom = DEVICE_DT_GET(DT_INST_PHANDLE(n, eeprom)),                               \
		.offset = DT_INST_PROP(n, offset),                                                 \
		.size = DT_INST_PROP(n, size),                                                     \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, retained_eeprom_init, NULL, NULL, &retained_eeprom_config_##n,    \
			      POST_KERNEL, CONFIG_ARDUINO_RETAINED_EEPROM_INIT_PRIORITY,           \
			      &retained_eeprom_api);

DT_INST_FOREACH_STATUS_OKAY(RETAINED_EEPROM_DEFINE)
